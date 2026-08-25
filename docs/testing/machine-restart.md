# Machine-restart durability test

Issue #4 requires a real restart of the backing machine after successful file and directory `fsync` calls.
The test must run on a disposable macOS or Linux host whose storage survives a hard reboot.

Run the prepare phase with an existing clean Git Project on that persistent storage:

```sh
TRIBIOS_BIN=/path/to/tribios \
TRIBIOS_DAEMON=/path/to/tribios_daemon \
tests/restart/machine_restart.sh prepare /persistent/project
```

Wait for `READY_TO_REBOOT`, then have the external host controller reset the machine without stopping Tribios.
Do not invoke the normal daemon shutdown path.

After the machine returns, run the verification phase against the same binaries and Project:

```sh
TRIBIOS_BIN=/path/to/tribios \
TRIBIOS_DAEMON=/path/to/tribios_daemon \
tests/restart/machine_restart.sh verify /persistent/project
```

The verification phase requires the synced file to contain the acknowledged bytes and requires recovery to have no pending operation.
Record the operating system, filesystem, Tribios commit, prepare output, reboot mechanism, and verification output as release evidence.
