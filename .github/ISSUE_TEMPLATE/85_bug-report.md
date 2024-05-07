---
name: Bug report
about: Report something broken in an Altinity Stable Build 
title: ''
labels: 'potential bug'
assignees: ''

---

*I checked [the Altinity Stable Builds lifecycle table](https://docs.altinity.com/altinitystablebuilds/#altinity-stable-builds-life-cycle-table), and the Altinity Stable Build version I'm using is still supported.*

## Describe what's wrong
A clear, concise description of what isn't working. 

> A link to reproducer in [https://fiddle.clickhouse.com/](https://fiddle.clickhouse.com/).

## Enable crash reporting

> Set `send_crash_reports\enabled` to `true` in `config.xml`:
```
<send_crash_reports>
        <!-- Changing <enabled> to true allows sending crash reports to -->
        <!-- the ClickHouse core developers team via Sentry https://sentry.io -->
        <enabled>true</enabled>
```

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
