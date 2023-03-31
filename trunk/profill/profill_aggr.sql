
PLAN 
select sum(d1) from d where d1 > 1 group by d1;

TRACE 
select sum(d1) from d where d1 > 1 group by d1;

EXPLAIN 
select sum(d1) from d where d1 > 1 group by d1;

DEBUG 
select sum(d1) from d where d1 > 1 group by d1;

PLAN
SELECT gender, AVG(score) AS avg_score
FROM students
WHERE age > 19
GROUP BY gender
HAVING AVG(score) > 90;

TRACE 
SELECT gender, AVG(score) AS avg_score
FROM students
WHERE age > 19
GROUP BY gender
HAVING AVG(score) > 90;

EXPLAIN
SELECT gender, AVG(score) AS avg_score
FROM students
WHERE age > 19
GROUP BY gender
HAVING AVG(score) > 90;
