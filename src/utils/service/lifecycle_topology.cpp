/**
 * @file lifecycle_topology.cpp
 * @brief Dependency graph construction and topological sort for the lifecycle subsystem.
 *
 * Included by lifecycle.cpp via the unified split model.
 * Private header: lifecycle_impl.hpp
 */
#include "lifecycle_impl.hpp"

namespace pylabhub::utils
{

// ============================================================================
// Graph construction and topology
// ============================================================================

/**
 * @brief Constructs the initial dependency graph from pre-registered static modules.
 * @details This function is called once during `initialize`. It populates the
 *          `m_module_graph` with all modules from `m_registered_modules`,
 *          then connects the dependency links between them.
 * @throws std::runtime_error If a duplicate module name is found or a
 *         dependency points to an undefined module.
 */
void LifecycleManagerImpl::buildStaticGraph()
{
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    for (const auto &def : m_registered_modules)
    {
        if (m_module_graph.contains(def.name))
        {
            throw std::runtime_error("Duplicate module name: " + def.name);
        }
        m_module_graph.emplace(std::piecewise_construct, std::forward_as_tuple(def.name),
                               std::forward_as_tuple(def.name, def.startup, def.shutdown,
                                                     def.dependencies, false /*is_dynamic*/,
                                                     def.is_persistent));
    }
    for (auto &entry : m_module_graph)
    {
        for (const auto &dep_name : entry.second.dependencies)
        {
            auto iter = m_module_graph.find(dep_name);
            if (iter == m_module_graph.end())
            {
                throw std::runtime_error("Undefined dependency: " + dep_name);
            }
            iter->second.dependents.push_back(&entry.second);
        }
    }
    m_registered_modules.clear();
}

/**
 * @brief Performs a topological sort on a given set of graph nodes.
 * @details Uses Kahn's algorithm to determine a valid linear ordering of nodes
 *          based on their dependencies.
 * @param nodes A vector of pointers to the nodes to be sorted.
 * @return A vector of node pointers in a valid topological order.
 * @throws std::runtime_error If a circular dependency is detected in the graph.
 */
std::vector<LifecycleManagerImpl::InternalGraphNode *>
LifecycleManagerImpl::topologicalSort(const std::vector<InternalGraphNode *> &nodes)
{
    std::vector<InternalGraphNode *> sorted_order;
    sorted_order.reserve(nodes.size());
    std::vector<InternalGraphNode *> zero_degree_queue;
    std::map<InternalGraphNode *, size_t> in_degrees;
    for (auto *node : nodes)
    {
        in_degrees[node] = 0;
    }
    for (auto *node : nodes)
    {
        for (auto *dep : node->dependents)
        {
            if (in_degrees.contains(dep))
            {
                in_degrees[dep]++;
            }
        }
    }
    for (auto *node : nodes)
    {
        if (in_degrees[node] == 0)
        {
            zero_degree_queue.push_back(node);
        }
    }
    size_t head = 0;
    while (head < zero_degree_queue.size())
    {
        InternalGraphNode *current = zero_degree_queue[head++];
        sorted_order.push_back(current);
        for (InternalGraphNode *dependent : current->dependents)
        {
            if (in_degrees.contains(dependent) && --in_degrees[dependent] == 0)
            {
                zero_degree_queue.push_back(dependent);
            }
        }
    }
    if (sorted_order.size() != nodes.size())
    {
        std::vector<std::string> cycle_nodes;
        for (auto const &[cycle_node, degree] : in_degrees)
        {
            if (degree > 0)
            {
                cycle_nodes.push_back(cycle_node->name);
            }
        }
        throw std::runtime_error("Circular dependency detected involving: " +
                                 fmt::format("{}", fmt::join(cycle_nodes, ", ")));
    }
    return sorted_order;
}

void LifecycleManagerImpl::printStatusAndAbort(const std::string &msg, const std::string &mod)
{
    fmt::print(stderr, "\n\n[PLH_LifeCycle] FATAL: {}. Aborting.\n", msg);
    if (!mod.empty())
    {
        fmt::print(stderr, "[PLH_LifeCycle] Module '{}' was point of failure.\n", mod);
    }
    fmt::print(stderr, "\n--- Module Status ---\n");
    for (auto const &[name, node] : m_module_graph)
    {
        fmt::print(stderr, "  - '{}' [{}]\n", name, node.is_dynamic ? "Dynamic" : "Static");
    }
    fmt::print(stderr, "---------------------\n\n");
    pylabhub::debug::print_stack_trace();
    std::fflush(stderr);
    std::abort();
}

} // namespace pylabhub::utils
