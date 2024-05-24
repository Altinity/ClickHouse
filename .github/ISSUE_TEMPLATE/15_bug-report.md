---
name: Bug report
about: Report something broken in an Altinity Stable Build 
title: ''
labels: 'potential bug'
assignees: ''

---

✅  *I checked [the Altinity Stable Builds lifecycle table](https://docs.altinity.com/altinitystablebuilds/#altinity-stable-builds-life-cycle-table), and the Altinity Stable Build version I'm using is still supported.*

## Type of problem
Choose one of the following items, then delete the others: 

**Bug report** - something's broken

**Incomplete implementation** - something's not quite right

**Performance issue** - something works, just not as quickly as it should

**Backwards compatibility issue** - something used to work, but now it doesn't

**Unexpected behavior** - something surprising happened, but it wasn't the good kind of surprise

**Installation issue** - something doesn't install they way it should

## Describe the situation
A clear, concise description of what's happening. 

## How to reproduce the behavior

* Which Altinity Stable Build version to use 
* Which interface to use, if it matters
* Non-default settings, if any
* `CREATE TABLE` statements for all tables involved
* Sample data for all these tables, use [clickhouse-obfuscator](https://github.com/ClickHouse/ClickHouse/blob/master/programs/obfuscator/Obfuscator.cpp#L42-L80) if necessary
* Queries to run that lead to an unexpected result

## Expected behavior
A clear, concise description of what you expected to happen.

## Logs, error messages, stacktraces, screenshots...
Add any details that might explain the issue.

## Additional context
Add any other context about the issue here.
