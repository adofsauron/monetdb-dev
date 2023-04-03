
TRACE
SELECT
*
FROM
c
WHERE
EXISTS (
    SELECT
    1
    FROM
    d
    WHERE
    c.c1 = d.d1
) ;


PLAN
SELECT
*
FROM
c
WHERE
EXISTS (
    SELECT
    1
    FROM
    d
    WHERE
    c.c1 = d.d1
) ;


EXPLAIN
SELECT
*
FROM
c
WHERE
EXISTS (
    SELECT
    1
    FROM
    d
    WHERE
    c.c1 = d.d1
) ;
