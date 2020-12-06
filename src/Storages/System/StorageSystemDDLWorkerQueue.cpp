#include <algorithm>
#include <string>

#include "StorageSystemDDLWorkerQueue.h"

#include <Columns/ColumnArray.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeString.h>
#include <Interpreters/Context.h>
#include <Common/ZooKeeper/ZooKeeper.h>


namespace DB
{
NamesAndTypesList StorageSystemDDLWorkerQueue::getNamesAndTypes()
{
    return {
        {"name", std::make_shared<DataTypeString>()}, // query_<id>
        {"active", std::make_shared<DataTypeArray>(std::make_shared<DataTypeString>())}, // list of host_fqdn
        {"finished", std::make_shared<DataTypeArray>(std::make_shared<DataTypeString>())}, // list of host_fqdn
    };
}


void StorageSystemDDLWorkerQueue::fillData(MutableColumns & res_columns, const Context & context, const SelectQueryInfo &) const
{
    zkutil::ZooKeeperPtr zookeeper = context.getZooKeeper();

    String ddl_zookeeper_path = config.getString("distributed_ddl.path", "/clickhouse/task_queue/ddl/");
    String ddl_query_path;

    // this is equivalent to query zookeeper at the `ddl_zookeeper_path`
    /* [zk: localhost:2181(CONNECTED) 51] ls /clickhouse/task_queue/ddl
       [query-0000000000, query-0000000001, query-0000000002, query-0000000003, query-0000000004]
    */
    zkutil::Strings queries = zookeeper->getChildren(ddl_zookeeper_path);

    // iterate through queries
    /*  Dir contents of every query will be similar to
     [zk: localhost:2181(CONNECTED) 53] ls /clickhouse/task_queue/ddl/query-0000000004
    [active, finished]
    */
    for (String q : queries)
    {
        /* Fetch all nodes under active & finished.
        [zk: localhost:2181(CONNECTED) 54] ls /clickhouse/task_queue/ddl/query-0000000004/active
        []
       [zk: localhost:2181(CONNECTED) 55] ls /clickhouse/task_queue/ddl/query-0000000004/finished
       [clickhouse01:9000, clickhouse02:9000, clickhouse03:9000, clickhouse04:9000]
       */
        size_t col_num = 0;
        ddl_query_path = ddl_zookeeper_path + "/" + q;
        zkutil::Strings active_nodes = zookeeper->getChildren(ddl_query_path + "/active");
        zkutil::Strings finished_nodes = zookeeper->getChildren(ddl_query_path + "/finished");

        res_columns[col_num++]->insert(q); // name - query_<id>

        {
            Array active_nodes_array;
            active_nodes_array.reserve(active_nodes.size());
            for (String active_node : active_nodes)
                active_nodes_array.emplace_back(active_node);
            res_columns[col_num++]->insert(active_nodes_array);
        }

        {
            Array finished_nodes_array;
            finished_nodes_array.reserve(active_nodes.size());
            for (String finished_node : finished_nodes)
                finished_nodes_array.emplace_back(finished_node);
            res_columns[col_num++]->insert(finished_nodes_array);
        }
    }
}
}
