

TRACE
SELECT b.b1,x.c1 FROM b 
LEFT JOIN 
(
    SELECT c1 FROM c INNER JOIN a ON a.a1 = c.c1
) x
ON b.b1 = x.c1;


PLAN
SELECT b.b1,x.c1 FROM b 
LEFT JOIN 
(
    SELECT c1 FROM c INNER JOIN a ON a.a1 = c.c1
) x
ON b.b1 = x.c1;


EXPLAIN
SELECT b.b1,x.c1 FROM b 
LEFT JOIN 
(
    SELECT c1 FROM c INNER JOIN a ON a.a1 = c.c1
) x
ON b.b1 = x.c1;
