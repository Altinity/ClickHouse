---
name: Backward compatibility issue
about: Describe a use case that used to work, but doesn't anymore
title: ''
labels: 'backward compatibility'
assignees: ''

---

## Describe the issue
A clear, concise description of what doesn't work anymore.

## How to reproduce the behavior

* The Altinity Stable Build version you're using now and the one you were using before
* Which interface to use, if matters
* Non-default settings, if any
* `CREATE TABLE` statements for all tables involved
* Sample data for all these tables, use [clickhouse-obfuscator](https://github.com/ClickHouse/ClickHouse/blob/master/programs/obfuscator/Obfuscator.cpp#L42-L80) if necessary
* Queries to run that lead to unexpected result

## Logs, error messages, stacktraces, screenshots...
Add any details that might explain the issue. Details from this use case as it ran under the previous version can be particularly helpful here. 

## Additional context
Add any other context about the issue here.
