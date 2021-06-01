###################################################################
#           Copyright (c) 2016 by TAOS Technologies, Inc.
#                     All rights reserved.
#
#  This file is proprietary and confidential to TAOS Technologies.
#  No part of this file may be reproduced, stored, transmitted,
#  disclosed or used in any form or by any means other than as
#  expressly provided by the written permission from Jianhui Tao
#
###################################################################

# -*- coding: utf-8 -*-

import sys
import os
from util.log import *
from util.cases import *
from util.sql import *
from util.dnodes import *


class TDTestCase:
    def init(self, conn, logSql):
        tdLog.debug("start to execute %s" % __file__)
        tdSql.init(conn.cursor(), logSql)
        
    def getBuildPath(self):
        selfPath = os.path.dirname(os.path.realpath(__file__))

        if ("community" in selfPath):
            projPath = selfPath[:selfPath.find("community")]
        else:
            projPath = selfPath[:selfPath.find("tests")]

        for root, dirs, files in os.walk(projPath):
            if ("taosd" in files):
                rootRealPath = os.path.dirname(os.path.realpath(root))
                if ("packaging" not in rootRealPath):
                    buildPath = root[:len(root)-len("/build/bin")]
                    break
        return buildPath
        
    def run(self):
        buildPath = self.getBuildPath()
        if (buildPath == ""):
            tdLog.exit("taosd not found!")
        else:
            tdLog.info("taosd found in %s" % buildPath)
        binPath = buildPath+ "/build/bin/"

        # insert: create one  or mutiple tables per sql and insert multiple rows per sql 
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-1s1tnt1r.json -y " % binPath)
        tdSql.execute("use db")
        tdSql.query("select count (tbname) from stb0")
        tdSql.checkData(0, 0, 1000)
        tdSql.query("select count (tbname) from stb1")
        tdSql.checkData(0, 0, 1000)
        tdSql.query("select count(*) from stb00_0")
        tdSql.checkData(0, 0, 100)
        tdSql.query("select count(*) from stb0")
        tdSql.checkData(0, 0, 100000)
        tdSql.query("select count(*) from stb01_1")
        tdSql.checkData(0, 0, 200)
        tdSql.query("select count(*) from stb1")
        tdSql.checkData(0, 0, 200000)        


        # insert: create  mutiple tables per sql and insert one rows per sql . 
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-1s1tntmr.json -y " % binPath)
        tdSql.execute("use db")
        tdSql.query("select count (tbname) from stb0")
        tdSql.checkData(0, 0, 10)
        tdSql.query("select count (tbname) from stb1")
        tdSql.checkData(0, 0, 20)
        tdSql.query("select count(*) from stb00_0")
        tdSql.checkData(0, 0, 10000)   
        tdSql.query("select count(*) from stb0")
        tdSql.checkData(0, 0, 100000) 
        tdSql.query("select count(*) from stb01_0")
        tdSql.checkData(0, 0, 20000)   
        tdSql.query("select count(*) from stb1")
        tdSql.checkData(0, 0, 400000) 

        # insert: using parament "insert_interval to controls spped  of insert. 
        # but We need to have accurate methods to control the speed, such as getting the speed value, checking the count and so on。
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-interval-speed.json -y" % binPath)
        tdSql.execute("use db")
        tdSql.query("show stables")
        tdSql.checkData(0, 4, 100)
        tdSql.query("select count(*) from stb00_0")
        tdSql.checkData(0, 0, 20000)   
        tdSql.query("select count(*) from stb0")
        tdSql.checkData(0, 0, 2000000) 
        tdSql.query("show stables")
        tdSql.checkData(1, 4, 100)
        tdSql.query("select count(*) from stb01_0")
        tdSql.checkData(0, 0, 20000)   
        tdSql.query("select count(*) from stb1")
        tdSql.checkData(0, 0, 2000000)           
        
        # spend 2min30s for 3 testcases.
        # insert: drop and child_table_exists combination test
        # insert: using parament "childtable_offset and childtable_limit" to control  table'offset point and offset 
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-nodbnodrop.json -y" % binPath)
        tdSql.error("show dbno.stables")
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-newdb.json -y" % binPath)
        tdSql.execute("use db")
        tdSql.query("select count (tbname) from stb0")
        tdSql.checkData(0, 0, 5)
        tdSql.query("select count (tbname) from stb1")
        tdSql.checkData(0, 0, 6)
        tdSql.query("select count (tbname) from stb2")
        tdSql.checkData(0, 0, 7)
        tdSql.query("select count (tbname) from stb3")
        tdSql.checkData(0, 0, 8)        
        tdSql.query("select count (tbname) from stb4")
        tdSql.checkData(0, 0, 8)  
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-offset.json -y" % binPath)
        tdSql.execute("use db")             
        tdSql.query("select count(*) from stb0")
        tdSql.checkData(0, 0, 50) 
        tdSql.query("select count(*) from stb1")
        tdSql.checkData(0, 0, 240) 
        tdSql.query("select count(*) from stb2")
        tdSql.checkData(0, 0, 220) 
        tdSql.query("select count(*) from stb3")
        tdSql.checkData(0, 0, 180)
        tdSql.query("select count(*) from stb4")
        tdSql.checkData(0, 0, 160)
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-newtable.json -y" % binPath)
        tdSql.execute("use db")             
        tdSql.query("select count(*) from stb0")
        tdSql.checkData(0, 0, 150) 
        tdSql.query("select count(*) from stb1")
        tdSql.checkData(0, 0, 360) 
        tdSql.query("select count(*) from stb2")
        tdSql.checkData(0, 0, 360) 
        tdSql.query("select count(*) from stb3")
        tdSql.checkData(0, 0, 340)
        tdSql.query("select count(*) from stb4")
        tdSql.checkData(0, 0, 400)
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-renewdb.json -y" % binPath)
        tdSql.execute("use db")             
        tdSql.query("select count(*) from stb0")
        tdSql.checkData(0, 0, 50) 
        tdSql.query("select count(*) from stb1")
        tdSql.checkData(0, 0, 120) 
        tdSql.query("select count(*) from stb2")
        tdSql.checkData(0, 0, 140) 
        tdSql.query("select count(*) from stb3")
        tdSql.checkData(0, 0, 160)
        tdSql.query("select count(*) from stb4")
        tdSql.checkData(0, 0, 160)


        # insert:  let parament in json file  is illegal, it'll expect error.
        tdSql.execute("drop database if exists db") 
        os.system("%staosdemo -f tools/taosdemoAllTest/insertColumnsAndTagNumLarge1024.json -y " % binPath)
        tdSql.error("use db")
        tdSql.execute("drop database if exists db") 
        os.system("%staosdemo -f tools/taosdemoAllTest/insertSigcolumnsNum1024.json -y " % binPath)
        tdSql.error("select * from db.stb0")
        tdSql.execute("drop database if exists db") 
        os.system("%staosdemo -f tools/taosdemoAllTest/insertColumnsAndTagNum1024.json -y " % binPath)
        tdSql.query("select count(*) from db.stb0")
        tdSql.checkData(0, 0, 10000) 
        tdSql.execute("drop database if exists db") 
        os.system("%staosdemo -f tools/taosdemoAllTest/insertColumnsNum0.json -y " % binPath)
        tdSql.execute("use db") 
        tdSql.query("show stables like 'stb0%' ")
        tdSql.checkData(0, 2, 11)
        tdSql.execute("drop database if exists db") 
        os.system("%staosdemo -f tools/taosdemoAllTest/insertTagsNumLarge128.json -y " % binPath)   
        tdSql.error("use db1") 
        tdSql.execute("drop database if exists db") 
        os.system("%staosdemo -f tools/taosdemoAllTest/insertBinaryLenLarge16374AllcolLar16384.json -y " % binPath)   
        tdSql.query("select count(*) from db.stb0") 
        tdSql.checkRows(1)
        tdSql.query("select count(*) from db.stb1") 
        tdSql.checkRows(1)
        tdSql.error("select * from db.stb3")
        tdSql.error("select * from db.stb2")
        tdSql.execute("drop database if exists db") 
        os.system("%staosdemo -f tools/taosdemoAllTest/insertNumOfrecordPerReq0.json -y " % binPath)   
        tdSql.error("select count(*) from db.stb0") 
        tdSql.execute("drop database if exists db") 
        os.system("%staosdemo -f tools/taosdemoAllTest/insertNumOfrecordPerReqless0.json -y " % binPath)   
        tdSql.error("use db") 
        tdSql.execute("drop database if exists db") 
        os.system("%staosdemo -f tools/taosdemoAllTest/insertChildTab0.json -y " % binPath)   
        tdSql.error("use db") 
        tdSql.execute("drop database if exists db") 
        os.system("%staosdemo -f tools/taosdemoAllTest/insertChildTabLess0.json -y " % binPath)   
        tdSql.error("use db") 
        tdSql.execute("drop database if exists blf") 
        os.system("%staosdemo -f tools/taosdemoAllTest/insertTimestepMulRowsLargeint16.json -y " % binPath)   
        tdSql.execute("use blf") 
        tdSql.query("select ts from blf.p_0_topics_7 limit 262800,1") 
        tdSql.checkData(0, 0, "2020-03-31 12:00:00.000")
        tdSql.query("select first(ts) from blf.p_0_topics_2")
        tdSql.checkData(0, 0, "2019-10-01 00:00:00")
        tdSql.query("select last(ts) from blf.p_0_topics_6 ")        
        tdSql.checkData(0, 0, "2020-09-29 23:59:00")



        # insert: timestamp and step 
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-timestep.json -y " % binPath)
        tdSql.execute("use db")
        tdSql.query("show stables")
        tdSql.query("select count (tbname) from stb0")
        tdSql.checkData(0, 0, 10)
        tdSql.query("select count (tbname) from stb1")
        tdSql.checkData(0, 0, 20)
        tdSql.query("select last(ts) from db.stb00_0")
        tdSql.checkData(0, 0, "2020-10-01 00:00:00.019000")   
        tdSql.query("select count(*) from stb0")
        tdSql.checkData(0, 0, 200) 
        tdSql.query("select last(ts) from db.stb01_0")
        tdSql.checkData(0, 0, "2020-11-01 00:00:00.190000")   
        tdSql.query("select count(*) from stb1")
        tdSql.checkData(0, 0, 400) 

        # # insert:  disorder_ratio
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-disorder.json -g 2>&1  -y " % binPath)
        tdSql.execute("use db")
        tdSql.query("select count (tbname) from stb0")
        tdSql.checkData(0, 0, 1)
        tdSql.query("select count (tbname) from stb1")
        tdSql.checkData(0, 0, 1)
        tdSql.query("select count(*) from stb0")
        tdSql.checkData(0, 0, 10) 
        tdSql.query("select count(*) from stb1")
        tdSql.checkData(0, 0, 10) 

        # insert:  sample json
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-sample.json -y " % binPath)
        tdSql.execute("use dbtest123")
        tdSql.query("select col2 from stb0")
        tdSql.checkData(0, 0, 2147483647)
        tdSql.query("select * from stb1 where t1=-127")
        tdSql.checkRows(20)
        tdSql.query("select * from stb1 where t2=127")
        tdSql.checkRows(10)
        tdSql.query("select * from stb1 where t2=126")
        tdSql.checkRows(10)

        # insert: test interlace parament 
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-interlace-row.json -y " % binPath)
        tdSql.execute("use db")
        tdSql.query("select count (tbname) from stb0")
        tdSql.checkData(0, 0, 100)
        tdSql.query("select count (*) from stb0")
        tdSql.checkData(0, 0, 15000)        

        # insert: auto_create
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-drop-exist-auto-YYY.json -y " % binPath) # drop = yes, exist = yes, auto_create = yes
        tdSql.execute('use db')
        tdSql.query('show tables')
        tdSql.checkRows(20)

        os.system("%staosdemo -f tools/taosdemoAllTest/insert-drop-exist-auto-YYN.json -y " % binPath) # drop = yes, exist = yes, auto_create = no
        tdSql.execute('use db')
        tdSql.query('show tables')
        tdSql.checkRows(20)

        os.system("%staosdemo -f tools/taosdemoAllTest/insert-drop-exist-auto-YNY.json -y " % binPath) # drop = yes, exist = no, auto_create = yes
        tdSql.execute('use db')
        tdSql.query('show tables')
        tdSql.checkRows(20)

        os.system("%staosdemo -f tools/taosdemoAllTest/insert-drop-exist-auto-YNN.json -y " % binPath) # drop = yes, exist = no, auto_create = no
        tdSql.execute('use db')
        tdSql.query('show tables')
        tdSql.checkRows(20)

        tdSql.execute('drop database db')
        tdSql.execute('create database db')
        tdSql.execute('use db')
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-drop-exist-auto-NYY.json -y " % binPath) # drop = no, exist = yes, auto_create = yes
        tdSql.query('show tables')
        tdSql.checkRows(0)

        tdSql.execute('drop database db')
        tdSql.execute('create database db')
        tdSql.execute('use db')
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-drop-exist-auto-NYN.json -y " % binPath) # drop = no, exist = yes, auto_create = no
        tdSql.execute('use db')
        tdSql.query('show tables')
        tdSql.checkRows(0)

        tdSql.execute('drop database db')
        tdSql.execute('create database db')
        tdSql.execute('use db')
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-drop-exist-auto-NNY.json -y " % binPath) # drop = no, exist = no, auto_create = yes
        tdSql.execute('use db')
        tdSql.query('show tables')
        tdSql.checkRows(20)

        tdSql.execute('drop database db')
        tdSql.execute('create database db')
        tdSql.execute('use db')
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-drop-exist-auto-NNN.json -y " % binPath) # drop = no, exist = no, auto_create = no
        tdSql.execute('use db')
        tdSql.query('show tables')
        tdSql.checkRows(20)

        #the following four test cases are for the exception cases for param auto_create_table

        tdSql.execute('drop database db')
        tdSql.execute('create database db')
        tdSql.execute('use db')
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-drop-exist-auto-NN123.json -y " % binPath) # drop = no, exist = no, auto_create = 123
        tdSql.execute('use db')
        tdSql.query('show tables')
        tdSql.checkRows(20)

        tdSql.execute('drop database db')
        tdSql.execute('create database db')
        tdSql.execute('use db')
        os.system("%staosdemo -f tools/taosdemoAllTest/insert-drop-exist-auto-NY123.json -y " % binPath) # drop = no, exist = yes, auto_create = 123
        tdSql.execute('use db')
        tdSql.query('show tables')
        tdSql.checkRows(0)

        os.system("%staosdemo -f tools/taosdemoAllTest/insert-drop-exist-auto-YN123.json -y " % binPath) # drop = yes, exist = no, auto_create = 123
        tdSql.execute('use db')
        tdSql.query('show tables')
        tdSql.checkRows(20)

        os.system("%staosdemo -f tools/taosdemoAllTest/insert-drop-exist-auto-YY123.json -y " % binPath) # drop = yes, exist = yes, auto_create = 123
        tdSql.execute('use db')
        tdSql.query('show tables')
        tdSql.checkRows(20)

        os.system("rm -rf ./insert_res.txt")
        os.system("rm -rf tools/taosdemoAllTest/taosdemoTestInsertWithJson.py.sql")        
        
        
        
    def stop(self):
        tdSql.close()
        tdLog.success("%s successfully executed" % __file__)


tdCases.addWindows(__file__, TDTestCase())
tdCases.addLinux(__file__, TDTestCase())
