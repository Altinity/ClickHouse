set force_index_by_date = 1;

create table xxxxx (col1 String, col2 String, _time DateTime, projection p (select * order by col2)) engine=MergeTree partition by col1 order by tuple();

create table yyyyyyy (col1 String, col2 String, _time DateTime, projection p (select * order by col2)) engine=MergeTree partition by col1 order by tuple();

insert into xxxxx (col1, col2, _time) values ('xxx', 'zzzz', now()+1);
insert into yyyyyyy (col1, col2, _time) values ('xxx', 'zzzz', now());

SELECT count()
FROM xxxxx
WHERE (col1 = 'xxx') AND (_time = (
    SELECT max(_time)
    FROM yyyyyyy
    WHERE (col1 = 'xxx') AND (col2 = 'zzzz') AND (_time > (now() - toIntervalDay(3)))))
