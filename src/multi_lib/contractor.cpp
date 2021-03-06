/*
 Cycle-routing does multi-criteria route planning for bicycles.
 Copyright (C) 2017  Florian Barth

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "contractor.hpp"
#include "contractionLP.hpp"
#include "multiqueue.hpp"
#include <any>
#include <chrono>
#include <iostream>
#include <memory>

class StatisticsCollector {
  public:
  enum class CountType { shortestPath, repeatingConfig, unknownReason };

  StatisticsCollector(bool active)
      : active(active) {};
  StatisticsCollector(const StatisticsCollector& other) = default;
  StatisticsCollector(StatisticsCollector&& other) noexcept = default;
  virtual ~StatisticsCollector() noexcept
  {
    if (!active || shortCount == 0) {
      return;
    }
    std::lock_guard guard(key);
    std::cout << shortCount << "\t\t" << sameCount << "\t\t\t" << unknown << "\t\t\t" << lpMax
              << "\t\t" << constMax << '\n';
  }
  StatisticsCollector& operator=(const StatisticsCollector& other) = default;
  StatisticsCollector& operator=(StatisticsCollector&& other) noexcept = default;

  static void printHeader()
  {
    std::cout << "| \t\t Reasons for shortcut creation \t\t | \t\t  Max values \t\t|  " << '\n';
    std::cout << "short \t\t repeating \t\t unknown \t\t lp calls \t max constraints" << '\n';
  }

  void countShortcut(CountType t)
  {
    switch (t) {
    case CountType::shortestPath: {
      ++shortCount;
      break;
    }
    case CountType::repeatingConfig: {
      ++sameCount;
      break;
    }
    case CountType::unknownReason: {
      ++unknown;
      break;
    }
    }
  }
  void recordMaxValues(size_t lpCalls, size_t constraints)
  {
    lpMax = std::max(lpCalls, lpMax);
    constMax = std::max(constraints, constMax);
  }

  protected:
  private:
  bool active;
  size_t shortCount = 0;
  size_t sameCount = 0;
  size_t unknown = 0;
  size_t lpMax = 0;
  size_t constMax = 0;
  static std::mutex key;
};
std::mutex StatisticsCollector::key {};

std::pair<bool, std::optional<RouteWithCount>> checkShortestPath(
    NormalDijkstra& d, const HalfEdge& startEdge, const HalfEdge& destEdge, const Config& conf)
{
  auto foundRoute = d.findBestRoute(startEdge.end, destEdge.end, conf);
  if (!foundRoute) {
    return std::make_pair(false, foundRoute);
  }

  auto shortcutCost = startEdge.cost + destEdge.cost;
  auto route = foundRoute.value();
  bool isShortest = route.costs == shortcutCost;
  return std::make_pair(isShortest, foundRoute);
}

class ContractingThread {
  MultiQueue<EdgePair>* queue;
  Graph* graph;
  StatisticsCollector stats;
  Config config;
  ContractionLp* lp;
  HalfEdge in;
  HalfEdge out;
  size_t lpCount = 0;
  NormalDijkstra d;
  std::vector<Edge> shortcuts;
  Cost shortcutCost;
  Cost currentCost;
  std::vector<Cost> constraints;
  RouteWithCount route;
  const std::set<NodePos>& set;

  public:
  ContractingThread(MultiQueue<EdgePair>* queue, Graph* g, const std::set<NodePos>& set,
      ContractionLp* lp, bool printStatistics)
      : queue(queue)
      , graph(g)
      , stats(printStatistics)
      , config(std::vector(Cost::dim, 1.0 / Cost::dim))
      , lp(lp)
      , d(g->createNormalDijkstra())
      , set(set)
  {
    shortcuts.reserve(graph->getNodeCount());
  }

  ContractingThread(const ContractingThread& c)
      : queue(c.queue)
      , graph(c.graph)
      , stats(c.stats)
      , config(c.config)
      , lp(c.lp)
      , d(c.d)
      , set(c.set)
  {
  }

  ContractingThread(const ContractingThread&& c)
      : queue(std::move(c.queue))
      , graph(std::move(c.graph))
      , stats(std::move(c.stats))
      , config(std::move(c.config))
      , lp(std::move(c.lp))
      , d(std::move(c.d))
      , set(std::move(c.set))
  {
  }

  bool isDominated(const Cost& costs)
  {
    bool dominated = true;
    bool someDifferent = false;
    for (size_t i = 0; i < Cost::dim; i++) {
      if (costs.values[i] > shortcutCost.values[i]) {
        dominated = false;
        someDifferent = true;
        break;
      }
      if (costs.values[i] != shortcutCost.values[i])
        someDifferent = true;
    }
    return dominated && someDifferent;
  }

  void addConstraint(const Cost& costs)
  {
    Cost newCost = costs - shortcutCost;
    lp->addConstraint(newCost.values);
  }

  void extractRoutesAndAddConstraints(RouteIterator& routes)
  {
    for (auto optRoute = routes.next(); optRoute; optRoute = routes.next()) {
      auto route = *optRoute;
      if (route.edges.size() == 2 && route.edges[0] == in.id && route.edges[1] == out.id) {
        continue;
      }
      addConstraint(route.costs);
      optRoute = routes.next();
    }
  }

  void storeShortcut(StatisticsCollector::CountType type)
  {
    stats.countShortcut(type);
    stats.recordMaxValues(lpCount, constraints.size());
    shortcuts.push_back(Contractor::createShortcut(Edge::getEdge(in.id), Edge::getEdge(out.id)));
  };

  bool testConfig(const Config& c)
  {
    auto [isShortest, foundRoute] = checkShortestPath(d, in, out, c);

    if (!foundRoute || foundRoute->edges.empty()) {
      stats.recordMaxValues(lpCount, constraints.size());
      return true;
    }

    currentCost = foundRoute->costs;
    route = *foundRoute;
    constraints.push_back(currentCost);

    if (isShortest) {
      if (route.pathCount == 1
          || std::any_of(route.edges.begin(), route.edges.end(), [this](const auto& id) {
               auto node = Edge::getEdge(id).destPos();
               return set.count(node) > 0;
             })) {
        storeShortcut(StatisticsCollector::CountType::shortestPath);
      }

      return true;
    }

    if (isDominated(currentCost)) {
      return true;
    }
    return false;
  }

  void dedupConstraints()
  {
    std::sort(constraints.begin(), constraints.end(),
        [](const auto& a, const auto& b) { return a.values < b.values; });

    auto last = std::unique(
        constraints.begin(), constraints.end(), [](const Cost& left, const Cost& right) {
          for (size_t i = 0; i < Cost::dim; ++i) {
            if (left.values[i] != right.values[i]) {
              return false;
            }
          }
          return true;
        });
    constraints.erase(last, constraints.end());
  }

  std::vector<Edge> operator()()
  {
    std::vector<EdgePair> messages;
    while (true) {
      messages.clear();
      if (queue->receive_some(messages, 20) == 0 && queue->closed()) {
        return std::move(shortcuts);
      }
      for (auto& pair : messages) {
        bool warm = false;
        if (pair.in.end == in.end && pair.out.end == out.end) {
          warm = true;
        } else {
          constraints.clear();
        }

        in = pair.in;
        out = pair.out;

        if (in.begin != out.begin) {
          throw std::invalid_argument("In out pair does not belong together");
        }
        auto in_edge = Edge::getMutEdge(in.id);
        auto out_edge = Edge::getMutEdge(out.id);

        if (in_edge.getDestId() != out_edge.getSourceId()) {
          throw std::invalid_argument("In out edges do not belong together");
        }

        std::vector<double> coeff(Cost::dim, 1.0 / Cost::dim);
        config = Config { coeff };
        shortcutCost = in.cost + out.cost;

        if (!warm) {
          warm = true;
          bool finished = false;
          for (size_t i = 0; i < Cost::dim; ++i) {
            std::vector<double> values(Cost::dim, 0);
            values[i] = 1;
            if (testConfig(values)) {
              finished = true;
              break;
            }
          }
          if (finished) {
            continue;
          }
        }

        for (auto c : constraints) {
          if (isDominated(c))
            continue;
        }

        lpCount = 0;
        while (true) {

          if (testConfig(config)) {
            break;
          }
          dedupConstraints();

          for (auto& c : constraints) {
            addConstraint(c);
          }

          ++lpCount;
          if (!lp->solve()) {
            stats.recordMaxValues(lpCount, constraints.size());
            break;
          }
          auto values = lp->variableValues();

          Config newConfig { values };
          if (newConfig == config) {
            if (currentCost * config >= shortcutCost * config - COST_ACCURACY) {
              storeShortcut(StatisticsCollector::CountType::repeatingConfig);
            } else {
              storeShortcut(StatisticsCollector::CountType::unknownReason);
            }
            break;
          }

          config = newConfig;
        }
      }
    }
  }
};

Contractor::Contractor(bool printStatistics)
    : Contractor(printStatistics,
        std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 1)
{
}

Contractor::Contractor(bool printStatistics, size_t maxThreads)
    : printStatistics(printStatistics)
    , THREAD_COUNT(maxThreads)
{

  while (lps.size() < THREAD_COUNT) {
    lps.push_back(std::make_unique<ContractionLp>());
  }
}

std::pair<bool, std::optional<RouteWithCount>> Contractor::isShortestPath(
    NormalDijkstra& d, const HalfEdge& startEdge, const HalfEdge& destEdge, const Config& conf)
{
  return checkShortestPath(d, startEdge, destEdge, conf);
}

Edge Contractor::createShortcut(const Edge& e1, const Edge& e2)
{
  if (e1.getDestId() != e2.getSourceId()) {
    throw std::invalid_argument(
        "Edges " + e1.external_id() + " and " + e2.external_id() + " are not connected");
  }
  Edge shortcut { e1.getSourceId(), e2.getDestId(), e1.getId(), e2.getId() };
  shortcut.setCost(e1.getCost() + e2.getCost());
  return shortcut;
}

std::future<std::vector<Edge>> Contractor::contract(
    MultiQueue<EdgePair>& queue, Graph& g, ContractionLp* lp, const std::set<NodePos>& set)
{
  return std::async(std::launch::async, ContractingThread { &queue, &g, set, lp, printStatistics });
}

std::set<NodePos> Contractor::independentSet(const Graph& g)
{
  std::set<NodePos> set;
  std::vector<std::pair<size_t, NodePos>> nodes;
  size_t nodeCount = g.getNodeCount();
  nodes.reserve(nodeCount);

  for (size_t i = 0; i < nodeCount; ++i) {
    NodePos p { i };
    auto inEdges = g.getIngoingEdgesOf(p);
    auto outEdges = g.getOutgoingEdgesOf(p);
    size_t count = (inEdges.end() - inEdges.begin()) * (outEdges.end() - outEdges.begin());
    nodes.emplace_back(count, p);
  }
  std::sort(nodes.begin(), nodes.end());

  std::vector<bool> selected(nodeCount, true);

  for (size_t i = 0; i < nodeCount; ++i) {
    NodePos pos = nodes[i].second;

    if (selected[pos]) {
      for (const auto& inEdge : g.getIngoingEdgesOf(pos)) {
        selected[inEdge.end] = false;
      }
      for (const auto& outEdge : g.getOutgoingEdgesOf(pos)) {
        selected[outEdge.end] = false;
      }
      set.insert(pos);
    }
  }
  std::cout << "..."
            << "calculated greedy independent set of " << set.size() << "\n";
  return set;
}

std::set<NodePos> Contractor::reduce(std::set<NodePos>& set, const Graph& g)
{
  std::vector<std::pair<size_t, NodePos>> metric {};
  metric.reserve(set.size());

  std::transform(set.begin(), set.end(), std::back_inserter(metric), [&g](NodePos p) {
    auto inEdges = g.getIngoingEdgesOf(p);
    auto outEdges = g.getOutgoingEdgesOf(p);
    size_t count = (inEdges.end() - inEdges.begin()) * (outEdges.end() - outEdges.begin());
    return std::make_pair(count, p);
  });
  size_t divider = 4;
  auto median
      = metric.begin() + (metric.size() < divider ? metric.size() : metric.size() / divider);

  std::nth_element(metric.begin(), median, metric.end());

  std::set<NodePos> result {};
  std::transform(metric.begin(), median, std::inserter(result, result.begin()),
      [](auto pair) { return std::get<NodePos>(pair); });

  std::cout << "..."
            << "reduced greedy independent set to " << result.size() << "\n";
  return result;
}

void copyEdgesOfNode(Graph& g, NodePos pos, std::vector<EdgeId>& edges)
{
  auto outRange = g.getOutgoingEdgesOf(pos);
  std::transform(outRange.begin(), outRange.end(), std::back_inserter(edges),
      [](const auto& e) { return e.id; });
  auto inRange = g.getIngoingEdgesOf(pos);
  std::transform(
      inRange.begin(), inRange.end(), std::back_inserter(edges), [](const auto e) { return e.id; });
}

Graph Contractor::contract(Graph& g)
{
  auto start = std::chrono::high_resolution_clock::now();
  MultiQueue<EdgePair> q {};

  ++level;
  auto set = reduce(independentSet(g), g);
  std::vector<std::future<std::vector<Edge>>> futures;
  for (size_t i = 0; i < THREAD_COUNT; ++i) {
    futures.push_back(contract(q, g, lps[i].get(), set));
  }
  std::vector<Node> nodes {};
  std::vector<EdgeId> edges {};
  std::vector<NodePos> nodesToContract {};
  nodesToContract.reserve(set.size());

  for (size_t i = 0; i < g.getNodeCount(); ++i) {
    NodePos pos { i };
    if (set.find(pos) == set.end()) {
      nodes.push_back(g.getNode(pos));
      for (const auto& edge : g.getOutgoingEdgesOf(pos)) {
        if (set.find(edge.end) == set.end()) {
          edges.push_back(edge.id);
        }
      }
    } else {
      nodesToContract.push_back(pos);

      Node node = g.getNode(pos);
      node.assignLevel(level);

      contractedNodes.push_back(node);
      copyEdgesOfNode(g, pos, contractedEdges);
    }
  }

  size_t edgePairCount = 0;
  size_t batchSize = THREAD_COUNT * 30;
  std::vector<EdgePair> pairs;
  pairs.reserve(batchSize);

  for (const auto& node : nodesToContract) {
    const auto& inEdges = g.getIngoingEdgesOf(node);
    const auto& outEdges = g.getOutgoingEdgesOf(node);
    for (const auto& in : inEdges) {
      for (const auto& out : outEdges) {
        if (in.end == out.end) {
          continue;
        }
        if (in.begin != out.begin) {
          throw std::invalid_argument("pair is not connecting");
        }
        pairs.push_back(EdgePair { in, out });
        ++edgePairCount;
        if (pairs.size() >= batchSize) {
          q.send(pairs);
        }
      }
    }
  }
  q.send(pairs);
  q.close();

  if (printStatistics) {
    std::cout << "..." << edgePairCount << " edge pairs to contract" << '\n';
    StatisticsCollector::printHeader();
  }

  std::vector<Edge> shortcuts {};
  for (size_t i = 0; i < futures.size(); ++i) {
    auto shortcutsMsg = futures[i].get();
    std::move(shortcutsMsg.begin(), shortcutsMsg.end(), std::back_inserter(shortcuts));
  }

  std::sort(shortcuts.begin(), shortcuts.end(), [](const auto& left, const auto& right) {
    if (left.getSourceId() < right.getSourceId())
      return true;
    if (left.getSourceId() > right.getSourceId())
      return false;

    if (left.getDestId() < right.getDestId())
      return true;
    if (left.getDestId() > right.getDestId())
      return false;

    for (size_t i = 0; i < Cost::dim; ++i) {
      if (left.getCost().values[i] < right.getCost().values[i])
        return true;
      if (left.getCost().values[i] > right.getCost().values[i])
        return false;
    }
    return false;
  });
  auto last
      = std::unique(shortcuts.begin(), shortcuts.end(), [](const auto& left, const auto& right) {
          bool sameNodes
              = left.getSourceId() == right.getSourceId() && left.getDestId() == right.getDestId();
          if (!sameNodes)
            return false;

          for (size_t i = 0; i < Cost::dim; ++i) {
            if (std::abs(left.getCost().values[i] - right.getCost().values[i]) > COST_ACCURACY) {
              return false;
            }
          }
          return true;
        });
  std::cout << "..."
            << "Erasing " << std::distance(last, shortcuts.end()) << " duplicate shortcuts."
            << '\n';
  shortcuts.erase(last, shortcuts.end());

  std::cout << "..."
            << "Created " << shortcuts.size() << " shortcuts." << '\n';
  auto ids = Edge::administerEdges(std::move(shortcuts));
  std::move(ids.begin(), ids.end(), std::back_inserter(edges));

  auto end = std::chrono::high_resolution_clock::now();

  using s = std::chrono::seconds;
  std::cout << "..."
            << "Last contraction step took " << std::chrono::duration_cast<s>(end - start).count()
            << "s" << '\n';
  shortcuts.clear();

  return Graph { std::move(nodes), std::move(edges) };
}

Graph Contractor::mergeWithContracted(Graph& g)
{
  std::vector<Node> nodes {};
  nodes.reserve(contractedNodes.size() + g.getNodeCount());
  std::move(contractedNodes.begin(), contractedNodes.end(), std::back_inserter(nodes));
  contractedNodes = std::vector<Node>();

  ++level;

  for (size_t i = 0; i < g.getNodeCount(); ++i) {
    NodePos pos { i };
    auto node = g.getNode(pos);
    node.assignLevel(level);
    nodes.push_back(node);
  }
  g = Graph(std::vector<Node>(), std::vector<Edge>());

  contractedEdges = std::vector<EdgeId>();

  std::vector<EdgeId> edges {};
  std::transform(Edge::edges.begin(), Edge::edges.end(), std::back_inserter(edges),
      [](auto& e) { return e.getId(); });

  std::cout << "Final graph has " << nodes.size() << " nodes and " << edges.size() << " edges."
            << '\n';

  return Graph { std::move(nodes), std::move(edges) };
}

Graph Contractor::contractCompletely(Graph& g, double rest)
{

  Graph intermedG = contract(g);
  double uncontractedNodesPercent
      = std::round(intermedG.getNodeCount() * 10000.0 / g.getNodeCount()) / 100;
  std::cout << 100 - uncontractedNodesPercent << "% of the graph is contracted ("
            << intermedG.getNodeCount() << " nodes left)" << '\n'
            << std::flush;
  while (uncontractedNodesPercent > rest) {
    intermedG = contract(intermedG);
    uncontractedNodesPercent
        = std::round(intermedG.getNodeCount() * 10000.0 / g.getNodeCount()) / 100;
    std::cout << "..."
              << "total number of edges: " << intermedG.getEdgeCount() + contractedEdges.size()
              << "\n";
    std::cout << 100 - uncontractedNodesPercent << "% of the graph is contracted ("
              << intermedG.getNodeCount() << " nodes left)" << '\n'
              << std::flush;
  }
  std::cout << '\n';
  return mergeWithContracted(intermedG);
}

Contractor::~Contractor() noexcept = default;
