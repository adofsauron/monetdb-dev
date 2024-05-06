

create table t1( a int, b int );
create table t2( a int, b int );
 
insert into t1(a,b) values(3,4);
insert into t1(a,b) values(5,6);
 
insert into t2(a,b) values(3,1);
insert into t2(a,b) values(7,2);


/* out join */
-- EXPLAIN / PLAN
SELECT * FROM  t1 LEFT JOIN t2 ON t2.a = t1.a;

/* null reject */
-- EXPLAIN / PLAN
SELECT * FROM  t1 LEFT JOIN t2 ON t2.a = t1.a WHERE t2.b < 5;

/* equal inner join */
-- EXPLAIN / PLAN
SELECT * FROM  t1 INNER JOIN t2 ON t2.a = t1.a WHERE t2.b < 5;
