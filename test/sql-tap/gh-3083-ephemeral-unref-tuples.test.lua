#!/usr/bin/env tarantool

test = require("sqltester")
test:plan(1)

test:do_test(
	"unref_before_delete-1.1",
	function()
		test:execsql "CREATE TABLE test(id integer, k integer, primary key (id))"

		for i = 1, 1000 do
			box.sql.execute("insert into test(id, k) values(" .. i .. "," .. i .. ")")
		end

		for i = 1, 10000 do
			box.sql.execute("SELECT id, k FROM test WHERE k IN (5849, 4986, 4997, 5020, 5044, 4990, 5013, 4983)")
		end
	end, {
		-- <unref_before_delete-1.1>
		-- </unref_before_delete-1.1>
	})

test:finish_test()

