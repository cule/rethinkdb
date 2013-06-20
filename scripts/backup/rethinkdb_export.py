#!/usr/bin/env python
import sys, os, datetime, time, threading, copy, json, traceback, csv
from optparse import OptionParser

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'drivers', 'python')))
import rethinkdb as r

usage = "'rethinkdb export` exports data from a rethinkdb cluster into a directory\n\
  rethinkdb export -c HOST:PORT [-a KEY] [-f (csv | json)] [-d DIR]\n\
      [--fields FIELD,FIELD...] [-e (DB | DB.TABLE)]..."

def print_export_help():
    print usage
    print ""
    print "  -h [ --help ]                    print this help"
    print "  -c [ --connect ] HOST:PORT       host and port of a rethinkdb node to connect to"
    print "  -a [ --auth ] AUTH_KEY           authorization key for rethinkdb clients"
    print "  -f [ --format ] (csv | json)     format to write (defaults to json)"
    print "  -d [ --directory ] DIR           directory to output to (defaults to"
    print "                                   rethinkdb_export_DATE_TIME)"
    print "  --fields FIELD,FIELD...          limit the exported fields to those specified"
    print "                                   (required for csv format)"
    print "  -e [ --export ] (DB | DB.TABLE)  limit dump to the given database or table (may"
    print "                                   be specified multiple times)"

def parse_options():
    parser = OptionParser(add_help_option=False, usage=usage)
    parser.add_option("-c", "--connect", dest="host", metavar="HOST:PORT", default="localhost:28015", type="string")
    parser.add_option("-a", "--auth", dest="auth_key", metavar="AUTHKEY", default="", type="string")
    parser.add_option("-f", "--format", dest="format", metavar="json | csv", default="json", type="string")
    parser.add_option("-d", "--directory", dest="directory", metavar="DIRECTORY", default=None, type="string")
    parser.add_option("-e", "--export", dest="tables", metavar="DB | DB.TABLE", default=[], action="append", type="string")
    parser.add_option("--fields", dest="fields", metavar="<FIELD>,<FIELD>...", default=None, type="string")
    parser.add_option("-h", "--help", dest="help", default=False, action="store_true")
    (options, args) = parser.parse_args()

    # Check validity of arguments
    if len(args) != 0:
        raise RuntimeError("no positional arguments supported")

    if options.help:
        print_export_help()
        exit(0)

    res = { }

    # Verify valid host:port --connect option
    host_port = options.host.split(":")
    if len(host_port) != 2:
        raise RuntimeError("invalid 'host:port' format")
    (res["host"], res["port"]) = host_port

    # Verify valid --format option
    if options.format not in ["csv", "json"]:
        raise RuntimeError("unknown format specified, valid options are 'csv' and 'json'")
    res["format"] = options.format

    # Verify valid directory option
    if options.directory is None:
        dirname = "./rethinkdb_export_%s" % datetime.datetime.today().strftime("%Y-%m-%dT%H:%M:%S")
    else:
        dirname = options.directory
    res["directory"] = os.path.abspath(dirname)

    if os.path.exists(res["directory"]):
        raise RuntimeError("output directory already exists")

    # Verify valid --export options
    res["tables"] = []
    for item in options.tables:
        db_table = item.split(".")
        if len(db_table) != 1 and len(db_table) != 2:
            raise RuntimeError("invalid 'db' or 'db.table' format: %s" % item)
        res["tables"].append(db_table)

    # Parse fields
    if options.fields is None:
        if options.format == "csv":
            raise RuntimeError("cannot write a csv with no fields selected")
        res["fields"] = None
    elif len(res["tables"]) != 1 or len(res["tables"][0]) != 2:
        raise RuntimeError("can only use the --fields option when exporting a single table")
    else:
        res["fields"] = options.fields.split(",")

    res["auth_key"] = options.auth_key
    return res

def get_tables(host, port, auth_key, tables):
    conn = r.connect(host, port, auth_key=auth_key)
    dbs = r.db_list().run(conn)
    res = []

    if len(tables) == 0:
        tables = [[db] for db in dbs]
    
    for db_table in tables:
        if db_table[0] not in dbs:
            raise RuntimeError("database '%s' not found" % db_table[0])

        if len(db_table) == 1: # This is just a db name
            res.extend([(db_table[0], table) for table in r.db(db_table[0]).table_list().run(conn)])
        else: # This is db and table name
            if db_table[1] not in r.db(db_table[0]).table_list().run(conn):
                raise RuntimeError("table not found: '%s.%s'" % db_table)
            res.append(tuple(db_table))
            
    # Remove duplicates by making results a set
    return set(res)

def prepare_directories(base_path, db_table_set):
    db_set = set([db for (db, table) in db_table_set])
    for db in db_set:
        os.makedirs(base_path + "/%s" % db)

def write_table_metadata(conn, db, table, base_path):
    out = open(base_path + "/%s/%s.info" % (db, table), "w")
    table_info = r.db(db).table(table).info().run(conn)
    out.write(json.dumps(table_info) + "\n")
    out.close()

def write_table_data_json(conn, db, table, base_path, fields):
    with open(base_path + "/%s/%s.json" % (db, table), "w") as out:
        first = True
        out.write("[")
        for row in r.db(db).table(table).run(conn):
            if fields is not None:
                for item in list(row.iterkeys()):
                    if item not in fields:
                        del row[item]
            if first:
                first = False
                out.write("\n" + json.dumps(row))
            else:
                out.write(",\n" + json.dumps(row))
        out.write("\n]\n")

def write_table_data_csv(conn, db, table, base_path, fields):
    with open(base_path + "/%s/%s.csv" % (db, table), "w") as out:
        out_writer = csv.writer(out) 
        out_writer.writerow(fields)

        for row in r.db(db).table(table).run(conn):
            info = [json.dumps(row[field]) for field in fields]
            out_writer.writerow(info)

def export_table(host, port, auth_key, db, table, directory, fields, errors, format):
    try:
        conn = r.connect(host, port, auth_key=auth_key)
        write_table_metadata(conn, db, table, directory)
        if format == "json":
            write_table_data_json(conn, db, table, directory, fields)
        elif format == "csv":
            write_table_data_csv(conn, db, table, directory, fields)
        else:
            raise RuntimeError("unknown format type: %s" % format)
    except:
        errors.append(sys.exc_info())

def run_clients(options, db_table_set):
    # Spawn one client for each db.table
    # TODO: do this in a more efficient manner
    threads = []
    errors = []

    for (db, table) in db_table_set:
        args = copy.deepcopy(options)
        args["db"] = db
        args["table"] = table
        args["errors"] = errors
        threads.append(threading.Thread(target=export_table, kwargs=args))
        threads[-1].start()

    # Wait for all tables to finish
    for thread in threads:
        thread.join()
    
    if len(errors) != 0:
        print "%d errors occurred:" % len(errors)
        for error in errors:
            traceback.print_exception(error[0], error[1], error[2])

def main():
    try:
        options = parse_options()
        db_table_set = get_tables(options["host"], options["port"], options["auth_key"], options["tables"])
        del options["tables"] # This is not needed anymore, db_table_set is more useful
        prepare_directories(options["directory"], db_table_set)
        start_time = time.time()
        run_clients(options, db_table_set)
    except RuntimeError as ex:
        print ex
        return 1
    print "Done (%d seconds)" % (time.time() - start_time)
    return 0

if __name__ == "__main__":
    main()
