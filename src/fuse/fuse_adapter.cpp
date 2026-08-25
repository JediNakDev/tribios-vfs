// The only operating-system specific file: FUSE callbacks in, Workspace engine
// calls out. No filesystem policy lives here. It speaks the FUSE 2.x API, which
// macFUSE implements on macOS and libfuse on Linux.
#define FUSE_USE_VERSION 26

#include "fuse/fuse_adapter.hpp"

#include <fuse.h>
#include <string.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>

#include "core/paths.hpp"
#include "core/proc.hpp"

namespace tribios {
namespace {

ProjectManager* glb_project_manager = nullptr;

// The view root has no backing directory of its own, so it gets a fixed inode.
// Workspace inodes come from the engines and are mixed across the 64-bit range,
// which keeps them clear of this one.
constexpr ino_t kProjectViewRootInode = 1;

// "/" is the view root, "/<workspace>" a Workspace root, deeper paths are
// Workspace-relative.
struct ViewPath {
  std::string workspace;
  std::string relative;
  bool is_root = false;
};

ViewPath parse_project_view_path(const char* path) {
  ViewPath view;
  const std::string normalized = normalize_relative(path ? path : "/");
  if (normalized.empty()) {
    view.is_root = true;
    return view;
  }
  const auto slash = normalized.find('/');
  if (slash == std::string::npos) {
    view.workspace = normalized;
    return view;
  }
  view.workspace = normalized.substr(0, slash);
  view.relative = normalized.substr(slash + 1);
  return view;
}

std::shared_ptr<WorkspaceEngine> find_workspace_engine_for_view_path(const ViewPath& view) {
  if (glb_project_manager == nullptr || view.workspace.empty()) return nullptr;
  return glb_project_manager->find_active_workspace_engine(view.workspace);
}

void populate_stat_from_attributes(const Attr& attr, struct stat* out) {
  memset(out, 0, sizeof(*out));
  out->st_ino = static_cast<ino_t>(attr.ino);
  out->st_mode = attr.mode;
  out->st_size = static_cast<off_t>(attr.size);
  out->st_nlink = static_cast<nlink_t>(attr.nlink);
  out->st_uid = getuid();
  out->st_gid = getgid();
  out->st_mtime = static_cast<time_t>(attr.mtime);
  out->st_atime = static_cast<time_t>(attr.atime);
  out->st_ctime = static_cast<time_t>(attr.ctime);
}

int tri_getattr(const char* path, struct stat* out) {
  const ViewPath view = parse_project_view_path(path);
  if (view.is_root) {
    memset(out, 0, sizeof(*out));
    out->st_ino = kProjectViewRootInode;
    out->st_mode = S_IFDIR | 0755;
    out->st_nlink = 2;
    out->st_uid = getuid();
    out->st_gid = getgid();
    return 0;
  }
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto attr = engine->getattr(view.relative);
  if (!attr) return -attr.error();
  populate_stat_from_attributes(*attr, out);
  return 0;
}

// Directory entries carry their inode and file type so that readers get a
// populated d_type. Without it every entry reads as DT_UNKNOWN and tools such
// as find, ls and git follow up with a separate lookup per entry.
int fill_directory_entry(void* buffer, fuse_fill_dir_t filler, const std::string& name,
                         std::uint64_t ino, mode_t mode) {
  struct stat entry_type {};
  entry_type.st_ino = static_cast<ino_t>(ino);
  entry_type.st_mode = mode;
  return filler(buffer, name.c_str(), &entry_type, 0);
}

// A Workspace root resolves through its own engine; anything deeper resolves
// relative to it. Returns the view root's inode for paths above a Workspace.
std::uint64_t resolve_view_path_inode(const ViewPath& view) {
  if (view.is_root) return kProjectViewRootInode;
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return kProjectViewRootInode;
  auto attr = engine->getattr(view.relative);
  return attr ? attr->ino : kProjectViewRootInode;
}

std::uint64_t parent_view_path_inode(const ViewPath& view) {
  if (view.is_root || view.relative.empty()) return kProjectViewRootInode;
  ViewPath parent = view;
  parent.relative = parent_of(view.relative);
  return resolve_view_path_inode(parent);
}

int tri_readdir(const char* path, void* buffer, fuse_fill_dir_t filler, off_t,
                struct fuse_file_info*) {
  const ViewPath view = parse_project_view_path(path);
  fill_directory_entry(buffer, filler, ".", resolve_view_path_inode(view), S_IFDIR);
  fill_directory_entry(buffer, filler, "..", parent_view_path_inode(view), S_IFDIR);
  if (view.is_root) {
    // The immediate children of the Project view are its visible Workspaces.
    for (const auto& name : glb_project_manager->active_workspace_names()) {
      ViewPath workspace_root;
      workspace_root.workspace = name;
      fill_directory_entry(buffer, filler, name, resolve_view_path_inode(workspace_root),
                           S_IFDIR);
    }
    return 0;
  }
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto entries = engine->readdir(view.relative);
  if (!entries) return -entries.error();
  for (const auto& entry : *entries) {
    fill_directory_entry(buffer, filler, entry.name, entry.ino, entry.mode);
  }
  return 0;
}

int tri_readlink(const char* path, char* buffer, size_t size) {
  const ViewPath view = parse_project_view_path(path);
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto target = engine->readlink(view.relative);
  if (!target) return -target.error();
  strncpy(buffer, target->c_str(), size - 1);
  buffer[size - 1] = '\0';
  return 0;
}

int tri_open(const char* path, struct fuse_file_info* info) {
  const ViewPath view = parse_project_view_path(path);
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto fd = engine->open_handle(view.relative, info->flags);
  if (!fd) return -fd.error();
  info->fh = static_cast<uint64_t>(*fd);
  return 0;
}

int tri_create(const char* path, mode_t mode, struct fuse_file_info* info) {
  const ViewPath view = parse_project_view_path(path);
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto created = engine->create(view.relative, mode);
  if (!created) return -created.error();
  auto fd = engine->open_handle(view.relative, O_RDWR);
  if (!fd) return -fd.error();
  info->fh = static_cast<uint64_t>(*fd);
  return 0;
}

int tri_read(const char* path, char* buffer, size_t size, off_t offset,
             struct fuse_file_info* info) {
  auto engine = find_workspace_engine_for_view_path(parse_project_view_path(path));
  if (engine == nullptr) return -ENOENT;
  auto n = engine->read_handle(static_cast<int>(info->fh), buffer, size,
                               static_cast<std::uint64_t>(offset));
  if (!n) return -n.error();
  return static_cast<int>(*n);
}

int tri_write(const char* path, const char* buffer, size_t size, off_t offset,
              struct fuse_file_info* info) {
  auto engine = find_workspace_engine_for_view_path(parse_project_view_path(path));
  if (engine == nullptr) return -ENOENT;
  auto n = engine->write_handle(static_cast<int>(info->fh), buffer, size,
                                static_cast<std::uint64_t>(offset));
  if (!n) return -n.error();
  return static_cast<int>(*n);
}

int tri_release(const char* path, struct fuse_file_info* info) {
  auto engine = find_workspace_engine_for_view_path(parse_project_view_path(path));
  if (engine != nullptr) engine->close_handle(static_cast<int>(info->fh));
  return 0;
}

int tri_mkdir(const char* path, mode_t mode) {
  const ViewPath view = parse_project_view_path(path);
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto status = engine->mkdir(view.relative, mode);
  return status ? 0 : -status.error();
}

int tri_unlink(const char* path) {
  const ViewPath view = parse_project_view_path(path);
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto status = engine->unlink(view.relative);
  return status ? 0 : -status.error();
}

int tri_rmdir(const char* path) {
  const ViewPath view = parse_project_view_path(path);
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto status = engine->rmdir(view.relative);
  return status ? 0 : -status.error();
}

int tri_rename(const char* from, const char* to) {
  const ViewPath from_view = parse_project_view_path(from);
  const ViewPath to_view = parse_project_view_path(to);
  auto engine = find_workspace_engine_for_view_path(from_view);
  if (engine == nullptr) return -ENOENT;
  // Isolation is a correctness guarantee, so renames never cross Workspaces.
  if (from_view.workspace != to_view.workspace) return -EXDEV;
  auto status = engine->rename(from_view.relative, to_view.relative);
  return status ? 0 : -status.error();
}

int tri_symlink(const char* target, const char* link_path) {
  const ViewPath view = parse_project_view_path(link_path);
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto status = engine->symlink(target, view.relative);
  return status ? 0 : -status.error();
}

int tri_chmod(const char* path, mode_t mode) {
  const ViewPath view = parse_project_view_path(path);
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto status = engine->chmod(view.relative, mode);
  return status ? 0 : -status.error();
}

int tri_chown(const char*, uid_t uid, gid_t gid) {
  // The Project is trusted, same-user infrastructure in this prototype.
  if (uid == getuid() && gid == getgid()) return 0;
  return -ENOTSUP;
}

int tri_truncate(const char* path, off_t size) {
  const ViewPath view = parse_project_view_path(path);
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto status = engine->truncate(view.relative, static_cast<std::uint64_t>(size));
  return status ? 0 : -status.error();
}

int tri_utimens(const char* path, const struct timespec tv[2]) {
  const ViewPath view = parse_project_view_path(path);
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto status = engine->utimens(view.relative, tv[0].tv_sec, tv[1].tv_sec);
  return status ? 0 : -status.error();
}

int tri_mknod(const char* path, mode_t mode, dev_t) {
  // Only regular files are Workspace contents; special files are unsupported.
  if (!S_ISREG(mode)) return -ENOTSUP;
  const ViewPath view = parse_project_view_path(path);
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto status = engine->create(view.relative, mode);
  return status ? 0 : -status.error();
}

// Unsupported semantics fail explicitly rather than silently misbehaving.
// macFUSE passes an extra offset to the xattr calls; Linux libfuse does not.
int tri_link(const char*, const char*) { return -ENOTSUP; }
#ifdef __APPLE__
int tri_setxattr(const char*, const char*, const char*, size_t, int, uint32_t) { return -ENOTSUP; }
int tri_getxattr(const char*, const char*, char*, size_t, uint32_t) { return -ENOTSUP; }
#else
int tri_setxattr(const char*, const char*, const char*, size_t, int) { return -ENOTSUP; }
int tri_getxattr(const char*, const char*, char*, size_t) { return -ENOTSUP; }
#endif
int tri_listxattr(const char*, char*, size_t) { return -ENOTSUP; }
int tri_removexattr(const char*, const char*) { return -ENOTSUP; }
int tri_lock(const char*, struct fuse_file_info*, int, struct flock*) { return -ENOTSUP; }

int tri_fsync(const char* path, int data_only, struct fuse_file_info* info) {
  if (info == nullptr) return -EBADF;
  auto engine = find_workspace_engine_for_view_path(parse_project_view_path(path));
  if (engine == nullptr) return -ENOENT;
  auto status = engine->fsync_handle(static_cast<int>(info->fh), data_only != 0);
  return status ? 0 : -status.error();
}

int tri_flush(const char* path, struct fuse_file_info* info) {
  if (info == nullptr) return -EBADF;
  auto engine = find_workspace_engine_for_view_path(parse_project_view_path(path));
  if (engine == nullptr) return -ENOENT;
  auto status = engine->fsync_handle(static_cast<int>(info->fh), false);
  return status ? 0 : -status.error();
}

int tri_fsyncdir(const char* path, int data_only, struct fuse_file_info*) {
  const ViewPath view = parse_project_view_path(path);
  auto engine = find_workspace_engine_for_view_path(view);
  if (engine == nullptr) return -ENOENT;
  auto status = engine->fsync_path(view.relative, data_only != 0, true);
  return status ? 0 : -status.error();
}

fuse_operations create_fuse_operations() {
  fuse_operations ops{};
  ops.getattr = tri_getattr;
  ops.readdir = tri_readdir;
  ops.readlink = tri_readlink;
  ops.open = tri_open;
  ops.create = tri_create;
  ops.read = tri_read;
  ops.write = tri_write;
  ops.release = tri_release;
  ops.mkdir = tri_mkdir;
  ops.unlink = tri_unlink;
  ops.rmdir = tri_rmdir;
  ops.rename = tri_rename;
  ops.symlink = tri_symlink;
  ops.chmod = tri_chmod;
  ops.chown = tri_chown;
  ops.truncate = tri_truncate;
  ops.utimens = tri_utimens;
  ops.mknod = tri_mknod;
  ops.link = tri_link;
  ops.setxattr = tri_setxattr;
  ops.getxattr = tri_getxattr;
  ops.listxattr = tri_listxattr;
  ops.removexattr = tri_removexattr;
  ops.lock = tri_lock;
  ops.flush = tri_flush;
  ops.fsync = tri_fsync;
  ops.fsyncdir = tri_fsyncdir;
  return ops;
}

}  // namespace

bool mount_supported() { return true; }

OutcomeVoid run_project_mount(ProjectManager& manager, const std::filesystem::path& mount_point,
                              bool debug) {
  glb_project_manager = &manager;
  std::error_code ec;
  std::filesystem::create_directories(mount_point, ec);

  // Multi-threaded on purpose: eight concurrent Workspaces must not queue
  // behind one another. default_permissions has the kernel enforce the modes
  // the engine reports, so permission changes behave like the host filesystem.
  // use_ino keeps the engine's own inode numbers instead of letting the FUSE
  // library invent one per lookup, which is what lets Git's index stay valid
  // once the kernel has evicted and re-looked-up a Workspace file.
  std::vector<std::string> arguments{"tribios",  mount_point.string(), "-f",
                                     "-o",       "default_permissions", "-o",
                                     "use_ino"};
#ifdef __APPLE__
  arguments.insert(arguments.end(), {"-o", "volname=Tribios", "-o", "noappledouble"});
#endif
  if (debug) arguments.push_back("-d");
  std::vector<char*> argv;
  for (auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));

  fuse_operations operations = create_fuse_operations();
  const int status = fuse_main(static_cast<int>(argv.size()), argv.data(), &operations, nullptr);
  glb_project_manager = nullptr;
  if (status != 0) return error("FUSE event loop exited with status " + std::to_string(status));
  return {};
}

void request_unmount(const std::filesystem::path& mount_point) {
#ifdef __APPLE__
  if (!run_process_and_capture_output({"umount", mount_point.string()}).ok()) {
    run_process_and_capture_output({"diskutil", "unmount", "force", mount_point.string()});
  }
#else
  // An unprivileged Linux user unmounts through fusermount.
  if (!run_process_and_capture_output({"fusermount", "-u", mount_point.string()}).ok()) {
    run_process_and_capture_output({"umount", mount_point.string()});
  }
#endif
}

}  // namespace tribios
