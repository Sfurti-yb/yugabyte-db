---
title: Python Drivers
linkTitle: Python Drivers
description: Python Drivers for YSQL
headcontent: Python Drivers for YSQL
image: /images/section_icons/sample-data/s_s1-sampledata-3x.png
menu:
  latest:
    name: Python Drivers
    identifier: postgres-psycopg2-driver
    parent: python-drivers
    weight: 500
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/drivers-orms/python/postgres-psycopg2/" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      PostgreSQL Psycopg2
    </a>
  </li>

</ul>

Psycopg is the most popular PostgreSQL database adapter for the Python programming language. Its main features are the complete implementation of the Python DB API 2.0 specification and the thread safety (several threads can share the same connection). YugabyteDB has full support for [Psycopg2](https://www.psycopg.org/).

## Quick Start

Learn how to establish a connection to YugabyteDB database and begin simple CRUD operations using the steps in [Build an Application](/latest/quick-start/build-apps/python/ysql-psycopg2) in the Quick Start section.

## Download the Driver Dependency

Building Psycopg requires a few prerequisites (a C compiler, some development packages): please check the [install](https://www.psycopg.org/docs/install.html#install-from-source) and the [faq](https://www.psycopg.org/docs/faq.html#faq-compile) documents in the doc dir or online for the details.

If prerequisites are met, you can install psycopg like any other Python package, using ``pip`` to download it from [PyPI](https://pypi.org/project/psycopg2/):
```
$ pip install psycopg2
```
or using ``setup.py`` if you have downloaded the source package locally:
```
$ python setup.py build
$ sudo python setup.py install
```
You can also obtain a stand-alone package, not requiring a compiler or external libraries, by installing the [psycopg2-binary](https://pypi.org/project/psycopg2-binary/) package from PyPI:
```
$ pip install psycopg2-binary
```
The binary package is a practical choice for development and testing but in production it is advised to use the package built from sources.

### Create a YugabyteDB Cluster

You can also setup a standalone YugabyteDB cluster by following the [install YugabyteDB Steps](/latest/quick-start/install/macos).

Alternatively, Set up a Free tier Cluster on [Yugabyte Anywhere](https://www.yugabyte.com/cloud/). The free cluster provides a fully functioning YugabyteDB cluster deployed to the cloud region of your choice. The cluster is free forever and includes enough resources to explore the core features available for developing the Java Applications with YugabyteDB database. Complete the steps for [creating a free tier cluster](latest/yugabyte-cloud/cloud-quickstart/qs-add/).

### Connect to YugabyteDB Database

Python Apps can connect to and query the YugabyteDB database. To do that first import the psycopg2 package. 
```python
import psycopg2
```
The Connection details can be provided as a string or a dictionary.
Connection String

```python
"dbname=database_name host=hostname port=port user=username  password=password"
```
Connection Dictionary
```python
user = 'username', password='xxx', host = 'hostname', port = 'port', dbname = 'database_name'
```

Example URL for connecting to YugabyteDB can be seen below.

```python
conn = psycopg2.connect(dbname='yugabyte',host='localhost',port='5433',user='yugabyte',password='yugabyte')
```

| Params | Description | Default |
| :---------- | :---------- | :------ |
| host  | hostname of the YugabyteDB instance | localhost
| port |  Listen port for YSQL | 5433
| database/dbname | database name | yugabyte
| user | user for connecting to the database | yugabyte
| password | password for connecting to the database | yugabyte

Example URL for connecting to YugabyteDB cluster enabled with on the wire SSL encryption.

```python
conn = psycopg2.connect("host=<hostname> port=5433 dbname=yugabyte user=<username> password=<password> sslmode=verify-full sslrootcert=/Users/my-user/Downloads/root.crt")
```

| Params | Description | Default |
| :---------- | :---------- | :------ |
| sslmode | SSL mode  | prefer
| sslrootcert | path to the root certificate on your computer | ~/.postgresql/

If you have created Free tier cluster on [Yugabyte Anywhere](https://www.yugabyte.com/cloud/), [Follow the steps](/latest/yugabyte-cloud/cloud-connect/connect-applications/) to download the Credentials and SSL Root certificate.

### Query the YugabyteDB Cluster from Your Application

Next, Create a new Python file called `QuickStartApp.py` in the base package directory of your project. Copy the sample code below in order to setup a YugbyteDB Tables and query the Table contents from the java client. Ensure you replace the connection string `yburl` with credentials of your cluster and SSL certs if required.
```python
import psycopg2

# Create the database connection.

yburl = "host=127.0.0.1 port=5433 dbname=yugabyte user=yugabyte password=yugabyte"

conn = psycopg2.connect(yburl)

# Open a cursor to perform database operations.
# The default mode for psycopg2 is "autocommit=false".

conn.set_session(autocommit=True)
cur = conn.cursor()

# Create the table. (It might preexist.)

cur.execute(
  """
  DROP TABLE IF EXISTS employee
  """)

cur.execute(
  """
  CREATE TABLE employee (id int PRIMARY KEY,
                         name varchar,
                         age int,
                         language varchar)
  """)
print("Created table employee")
cur.close()

# Take advantage of ordinary, transactional behavior for DMLs.

conn.set_session(autocommit=False)
cur = conn.cursor()

# Insert a row.

cur.execute("INSERT INTO employee (id, name, age, language) VALUES (%s, %s, %s, %s)",
            (1, 'John', 35, 'Python'))
print("Inserted (id, name, age, language) = (1, 'John', 35, 'Python')")

# Query the row.

cur.execute("SELECT name, age, language FROM employee WHERE id = 1")
row = cur.fetchone()
print("Query returned: %s, %s, %s" % (row[0], row[1], row[2]))

# Commit and close down.

conn.commit()
cur.close()
conn.close()
```
When you run the Project, `QuickStartApp.py` should output something like below:

```text
Created table employee
Inserted (id, name, age, language) = (1, 'John', 35, 'Python')
Query returned: John, 35, Python
```

if you receive no output or error, check whether you included the proper connection string with the right credentials.

After completing these steps, you should have a working Python app that uses Psycopg2 for connecting to your cluster, setup tables, run query and print out results.

## Working with Domain Objects (ORMs)

In the previous section, you ran a SQL query on a sample table and displayed the results set. In this section, we'll learn to use the ORMs to store and retrieve data from YugabyteDB Cluster.

An Object Relational Mapping (ORM) tool is used by the developers to handle database access, it allows developers to map their object-oriented domain classes into the database tables. It simplifies the CRUD operations on your domain objects and easily allow the evolution of Domain objects to be applied to the Database tables.

[SQLAlchemy](https://www.sqlalchemy.org/) is a popular ORM provider for Python applications which is widely used by Python Developers for Database access. YugabyteDB provides full support for SQLAlchemy ORM.

### Add the SQLAlchemy ORM Dependency

To download and install SQLAlchemy to your project, use the following command.
```shell
  pip3 install sqlalchemy
  ```

  You can verify the installation as follows: 

  - Open the Python prompt by executing the following command:

    ```shell
    python3
    ```

  - From the Python prompt, execute the following commands to check the SQLAlchemy version:

    ```python prompt
    import sqlalchemy
    ```

    ```python prompt
    sqlalchemy.__version__
    ```

### Implementing ORM mapping for YugabyteDB

To start with SQLAlchemy, in your project directory, create 4 Python files - `config.py`,`base.py`,`model.py` and `main.py`

The `config.py` will contain the credentials to connect to your database. Copy the following sample code to your `config.py` file.

```python
  db_user = 'yugabyte'
  db_password = 'yugabyte'
  database = 'yugabyte'
  db_host = 'localhost'
  db_port = 5433
```
Next step is to declare a mapping.When using the ORM, the configurational process starts by describing the database tables we’ll be dealing with, and then by defining our own classes which will be mapped to those tables. In modern SQLAlchemy, these two tasks are usually performed together, using a system known as `Declarative Extensions`. Classes mapped using the Declarative system are defined in terms of a base class which maintains a catalog of classes and tables relative to that base - this is known as the declarative base class. Create the Base class using the `declarative_base()` function. For this in the `base.py` add the following code.

```python
from sqlalchemy.ext.declarative import declarative_base
Base = declarative_base()
```

Now that we have a “base”, we can define any number of mapped classes in terms of it. We will start with just a single table called `employees`, which will store records for the end-users using your application. A new class called `Employee` will be the class to which we map this table. Within the class, we define details about the table to which we’ll be mapping, primarily the table name, and names and datatypes of columns. In the `model.py` add:

```python
from sqlalchemy.ext.declarative import declarative_base
from sqlalchemy import Table, Column, Integer, String, DateTime, ForeignKey
from base import Base

class Employee(Base):

   __tablename__ = 'employees'

   id = Column(Integer, primary_key=True)
   name = Column(String(255), unique=True, nullable=False)
   age = Column(Integer)
   language = Column(String(255))
```

Once all of the setup is done. We will connect to our database and create a new session. In the `main.py` add the following saple code.

```python
import config as cfg
from sqlalchemy.orm import sessionmaker, relationship
from sqlalchemy import create_engine
from sqlalchemy import MetaData
from model import Employee
from base import Base
from sqlalchemy import Table, Column, Integer, String, DateTime, ForeignKey


# create connection
engine = create_engine('postgresql://{0}:{1}@{2}:{3}/{4}'.format(cfg.db_user, cfg.db_password, cfg.db_host, cfg.db_port, cfg.database))

# create metadata
Base.metadata.create_all(engine)

# create session
Session = sessionmaker(bind=engine)
session = Session()

# insert data
tag_1 = Employee(name='Bob', age=21, language='Python')
tag_2 = Employee(name='John', age=35, language='Java')
tag_3 = Employee(name='Ivy', age=27, language='C++')

session.add_all([tag_1, tag_2, tag_3])

print('Query returned:')
for instance in session.query(Employee):
    print("Name: %s Age: %s Language: %s"%(instance.name, instance.age, instance.language))
session.commit()

```
When you run the `main.py` file, you will get the fllowing output.

```text
Query returned:
Name: Bob Age: 21 Language: Python
Name: John Age: 35 Language: Java
Name: Ivy Age: 27 Language: C++
```

## Next Steps
