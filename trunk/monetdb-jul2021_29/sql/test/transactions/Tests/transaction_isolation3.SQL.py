from MonetDBtesting.sqltest import SQLTestCase

with SQLTestCase() as mdb1:
    with SQLTestCase() as mdb2:
        mdb1.connect(username="monetdb", password="monetdb")
        mdb2.connect(username="monetdb", password="monetdb")

        mdb1.execute("create table w (i int);").assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("alter table w add column j int;").assertSucceeded()
        mdb2.execute("alter table w add column j int;").assertFailed(err_code="42000", err_message="ALTER TABLE: sys_w_j conflicts with another transaction") # duplicate column
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('rollback;').assertSucceeded()

        mdb1.execute("CREATE TABLE notpossible (i int, j int);").assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute('insert into notpossible values (5,1),(5,2),(5,3);').assertSucceeded()
        mdb2.execute('alter table notpossible add primary key (i);').assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute("CREATE TABLE integers (i int, j int);").assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute('alter table integers add primary key (i);').assertSucceeded()
        mdb2.execute('insert into integers values (5,1),(5,2),(5,3);').assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute('insert into integers values (6,NULL),(7,NULL),(8,NULL);').assertSucceeded()
        mdb2.execute('alter table integers alter j set not null;').assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute('truncate table integers;').assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute('alter table integers alter j set not null;').assertSucceeded()
        mdb2.execute('insert into integers values (6,NULL),(7,NULL),(8,NULL);').assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute('create schema ups;').assertSucceeded()
        mdb1.execute('create merge table parent1(a int) PARTITION BY RANGE ON (a);').assertSucceeded()
        mdb1.execute('create table child1(c int);').assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("ALTER TABLE parent1 ADD TABLE child1 AS PARTITION FROM '1' TO '2';").assertSucceeded()
        mdb2.execute("alter table child1 set schema ups;").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute('create merge table parent2(a int) PARTITION BY RANGE ON (a);').assertSucceeded()
        mdb1.execute('create table child2(c int);').assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("ALTER TABLE parent2 ADD TABLE child2 AS PARTITION FROM '1' TO '2';").assertSucceeded()
        mdb2.execute("insert into child2 values (3);").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute('create table x(y int, z int);').assertSucceeded()
        mdb1.execute('insert into x values (1, 1);').assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("create view myv(a,b) as select y, z from x;").assertSucceeded()
        mdb2.execute("alter table x drop column y;").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")
        mdb1.execute('select * from myv;').assertSucceeded().assertDataResultMatch([(1,1)])

        mdb1.execute("create table ups.no (a int, b int);").assertSucceeded()
        mdb1.execute('insert into ups.no values (2, 2);').assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("create function sys.another() returns table(i int) begin return select a from ups.no; end;").assertSucceeded()
        mdb2.execute("alter table ups.no drop column a;").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")
        mdb1.execute('select * from another();').assertSucceeded().assertDataResultMatch([(2,)])

        mdb1.execute("CREATE TABLE y (i int);").assertSucceeded()
        mdb1.execute('CREATE TABLE integers2 (i int, j int);').assertSucceeded()
        mdb1.execute('insert into integers2 values (1,1),(2,2),(3,3);').assertSucceeded()
        mdb1.execute('alter table integers2 add primary key (i);').assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("alter table y add constraint nono foreign key(i) references integers2(i);").assertSucceeded()
        mdb2.execute("insert into y values (4);").assertSucceeded() # violates foreign key if mdb1 committed successfully
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute("create function pain() returns int return 1;").assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("create view myv2(a) as select pain();").assertSucceeded()
        mdb2.execute("drop function pain();").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")
        mdb1.execute('select * from myv2;').assertSucceeded().assertDataResultMatch([(1,)])

        mdb1.execute("CREATE TABLE longs (i bigint);").assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("create or replace trigger myt after insert on integers referencing new row as new_row for each row insert into longs values(16);").assertSucceeded()
        mdb2.execute("drop table longs;").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")
        mdb1.execute('insert into integers values (4,4);').assertSucceeded()
        mdb1.execute('select * from longs;').assertSucceeded().assertDataResultMatch([(16,)])

        mdb1.execute("create table z (i int);").assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("create view myv3(a) as select i from z;").assertSucceeded()
        mdb2.execute("alter table z rename to zz;").assertSucceeded() # myv3 uses the table
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute("create table zzz (i int);").assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("alter table zzz rename to aaa;").assertSucceeded()
        mdb2.execute("create view myv8(a) as select i from zzz;").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute('create table ww(y int, z int);').assertSucceeded()
        mdb1.execute('insert into ww values (1, 1);').assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("create view myv4(a,b) as select y, z from ww;").assertSucceeded()
        mdb2.execute("alter table ww rename column y to yy;").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")
        mdb1.execute('select * from myv4;').assertSucceeded().assertDataResultMatch([(1,1)])

        mdb1.execute('create table bbb(y int, z int);').assertSucceeded()
        mdb1.execute('insert into bbb values (1, 1);').assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("alter table bbb rename column y to yy;").assertSucceeded()
        mdb2.execute("create view myv9(a,b) as select y, z from bbb;").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute('create table zz(y int, z int);').assertSucceeded()
        mdb1.execute('insert into zz values (1, 1);').assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("create view myv5(a,b) as select y, z from zz;").assertSucceeded()
        mdb2.execute("alter table zz set schema ups;").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")
        mdb1.execute('select * from myv5;').assertSucceeded().assertDataResultMatch([(1,1)])

        mdb1.execute('create table xx(y int, z int);').assertSucceeded()
        mdb1.execute('insert into xx values (1, 1);').assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("alter table xx set schema ups;").assertSucceeded()
        mdb2.execute("create view myv6(a,b) as select y, z from sys.xx;").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute('create table fine(y int, z int);').assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("alter table fine drop column y;").assertSucceeded()
        mdb2.execute("create view myv7(a,b) as select y, z from sys.fine;").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute('create table fine2(y int, z int);').assertSucceeded()
        mdb1.execute('start transaction;').assertSucceeded()
        mdb2.execute('start transaction;').assertSucceeded()
        mdb1.execute("create view myv10(a,b) as select y, z from fine2;").assertSucceeded()
        mdb2.execute("alter table fine2 drop column y;").assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()
        mdb2.execute('commit;').assertFailed(err_code="40000", err_message="COMMIT: transaction is aborted because of concurrency conflicts, will ROLLBACK instead")

        mdb1.execute('start transaction;').assertSucceeded()
        mdb1.execute('DROP TABLE w;').assertSucceeded()
        mdb1.execute('drop table notpossible;').assertSucceeded()
        mdb1.execute('drop table y;').assertSucceeded()
        mdb1.execute('drop table integers2;').assertSucceeded()
        mdb1.execute('drop trigger myt;').assertSucceeded()
        mdb1.execute('drop table longs;').assertSucceeded()
        mdb1.execute('drop table integers;').assertSucceeded()
        mdb1.execute('drop function another;').assertSucceeded()
        mdb1.execute('DROP VIEW myv5;').assertSucceeded()
        mdb1.execute('drop table zz;').assertSucceeded()
        mdb1.execute('drop table ups.xx;').assertSucceeded()
        mdb1.execute('drop table ups.no;').assertSucceeded()
        mdb1.execute('drop schema ups;').assertSucceeded()
        mdb1.execute('ALTER TABLE parent1 DROP TABLE child1;').assertSucceeded()
        mdb1.execute('DROP TABLE parent1;').assertSucceeded()
        mdb1.execute('DROP TABLE child1;').assertSucceeded()
        mdb1.execute('DROP TABLE parent2;').assertSucceeded()
        mdb1.execute('DROP TABLE child2;').assertSucceeded()
        mdb1.execute('DROP VIEW myv;').assertSucceeded()
        mdb1.execute('DROP TABLE x;').assertSucceeded()
        mdb1.execute('DROP VIEW myv3;').assertSucceeded()
        mdb1.execute('DROP TABLE z;').assertSucceeded()
        mdb1.execute('DROP VIEW myv4;').assertSucceeded()
        mdb1.execute('DROP TABLE ww;').assertSucceeded()
        mdb1.execute('DROP VIEW myv2;').assertSucceeded()
        mdb1.execute('DROP FUNCTION pain();').assertSucceeded()
        mdb1.execute('DROP TABLE fine;').assertSucceeded()
        mdb1.execute('DROP TABLE aaa;').assertSucceeded()
        mdb1.execute('DROP TABLE bbb;').assertSucceeded()
        mdb1.execute('DROP VIEW myv10;').assertSucceeded()
        mdb1.execute('DROP TABLE fine2;').assertSucceeded()
        mdb1.execute('commit;').assertSucceeded()