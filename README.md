# mutrace

A lock usage and contention profiler.

Understands pthread mutexes, condition variables, and rwlocks.

Also understands ISC rwlocks.


# matrace

matrace traces memory allocation operations in realtime threads.

Only useful for applications that use realtime scheduling.


# Configuration, Build, and Install

```bash
# Configure
./bootstrap.sh

# Make and install
make && sudo make install
```

# Usage

Both utilities work using LD_PRELOD and support multiple options
affecting data collection

See the individual help for each utility for more details.

```bash
# Help
matrace -h
mutrace -h

# Run
matrace [matrace-options] <command>
mutrace [mutrace-options] <command>
```
