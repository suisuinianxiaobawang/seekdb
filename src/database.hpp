#pragma once
#include "store.hpp"
#include "distance.hpp"
#include <optional>
#include <queue>
#include <unordered_map>

namespace mini {
enum class Op : uint8_t { Create, Put, Delete, Index };
enum class Compare { Eq, Lt, Le, Gt, Ge };
using Cell = std::variant<int64_t, std::string_view, double>;

struct Row {
    std::string_view vector;
    std::vector<Cell> fields;
};

struct Distance {
    size_t column;
    std::vector<float> target;
    bool operator==(const Distance&) const = default;
};

struct Filter {
    size_t column;
    Compare op;
    Value expected;

    bool match(const Row& row) const {
        const auto& value = row.fields[column];
        int cmp;
        if (const auto* number = std::get_if<int64_t>(&expected)) {
            auto actual = std::get<int64_t>(value);
            cmp = (actual > *number) - (actual < *number);
        } else cmp = std::get<std::string_view>(value).compare(std::get<std::string>(expected));
        switch (op) {
            case Compare::Eq: return cmp == 0;
            case Compare::Lt: return cmp < 0;
            case Compare::Le: return cmp <= 0;
            case Compare::Gt: return cmp > 0;
            case Compare::Ge: return cmp >= 0;
        }
        return false;
    }
};

struct Expression {
    size_t column;
    std::optional<Distance> distance;
    std::string label;
};
struct Result {
    bool query = false;
    size_t affected = 0;
    std::vector<std::string> columns;
    size_t row_count = 0;
    using Emit = std::function<void(const std::vector<std::string>&)>;
    std::function<void(const Emit&)> emit;
};

class Database {
    Log log;
    std::map<std::string, Table> tables;
    std::vector<Table*> ids;
    std::vector<std::string> created;
    std::vector<Table*> indexed;
    std::set<Table*> changed;
    bool transaction = false;

    struct IndexEntry {
        int64_t key;
        uint64_t offset;
    };
    struct FlatIndex {
        const Table* table = nullptr;
        std::vector<IndexEntry> entries;
        std::optional<size_t> filter_column;
        std::unordered_map<int64_t, std::vector<IndexEntry>> numeric;
        std::map<std::string, std::vector<IndexEntry>, std::less<>> text;
    } index;
    static constexpr size_t index_rows = 32768;
    static constexpr size_t index_text_bytes = 512 * 1024;

    void invalidate_index(const Table& t) {
        if (index.table == &t) index = {};
    }

    Table& table(const std::string& name) {
        auto it = tables.find(name);
        if (it == tables.end()) throw Error("UNKNOWN_TABLE", "Unknown table " + name);
        return it->second;
    }
    void add_table(std::string name, Table value) {
        auto [it, added] = tables.emplace(std::move(name), std::move(value));
        if (!added || it->second.id != ids.size()) throw std::runtime_error("Invalid table catalog");
        ids.push_back(&it->second);
    }
    void recover(uint64_t offset, std::string_view bytes) {
        Reader data(bytes);
        auto op = data.get<Op>();
        auto id = data.get<uint32_t>();
        if (op == Op::Create) {
            auto name = std::string(data.string());
            auto primary = data.get<uint32_t>();
            auto count = data.get<uint32_t>();
            std::vector<Column> columns;
            for (uint32_t i = 0; i < count; ++i) {
                auto type = data.get<Type>();
                auto size = data.get<uint32_t>();
                columns.push_back({std::string(data.string()), type, size});
            }
            add_table(std::move(name), Table{id, std::move(columns), primary, {}, {}, {}});
            return;
        }
        auto& t = *ids.at(id);
        if (op == Op::Put) t.rows[data.get<int64_t>()] = offset;
        else if (op == Op::Delete) t.rows.erase(data.get<int64_t>());
        else if (op == Op::Index) t.index = data.string();
        else throw std::runtime_error("Invalid storage operation");
    }
    void commit() {
        log.commit();
        for (Table* t : changed) {
            for (auto it = t->pending.begin(); it != t->pending.end();) {
                auto node = t->pending.extract(it++);
                if (!node.mapped()) t->rows.erase(node.key());
                else {
                    auto existing = t->rows.find(node.key());
                    if (existing != t->rows.end()) existing->second = node.mapped();
                    else t->rows.insert(std::move(node));
                }
            }
        }
        changed.clear();
        indexed.clear();
        created.clear();
    }
    void rollback() {
        log.rollback();
        for (Table* t : changed) t->pending.clear();
        for (Table* t : indexed) {
            invalidate_index(*t);
            t->index.clear();
        }
        for (auto it = created.rbegin(); it != created.rend(); ++it) {
            ids.pop_back();
            tables.erase(*it);
        }
        changed.clear();
        indexed.clear();
        created.clear();
    }
    static Value value(Parser& parser, const Column& column, bool check_length = true) {
        if (column.type == Type::Bigint) return parser.integer();
        if (column.type == Type::Vector) return parser.vector(column.size);
        auto s = parser.string();
        auto length = unicode_length(s);
        if (check_length && length > column.size) invalid("VARCHAR too long");
        return s;
    }
    static std::string record_header(Op op, uint32_t table_id) {
        std::string out;
        put(out, op);
        put(out, table_id);
        return out;
    }
    static std::string encode(const Table& t, const std::vector<Value>& values) {
        auto out = record_header(Op::Put, t.id);
        put(out, std::get<int64_t>(values[t.primary]));
        for (const auto& v : values) if (const auto* vec = std::get_if<std::vector<float>>(&v))
            out.append(reinterpret_cast<const char*>(vec->data()), vec->size() * sizeof(float));
        for (size_t i = 0; i < values.size(); ++i) {
            if (i == t.primary) continue;
            if (const auto* n = std::get_if<int64_t>(&values[i])) put(out, *n);
            else if (const auto* s = std::get_if<std::string>(&values[i])) put_string(out, *s);
        }
        return out;
    }
    static size_t vector_size(const Table& t) {
        for (const auto& column : t.columns)
            if (column.type == Type::Vector) return size_t(column.size) * sizeof(float);
        return 0;
    }
    void decode(const Table& t, uint64_t offset, size_t vector_bytes, Row& row, bool fields) {
        Reader data(log.record(offset));
        data.take(sizeof(Op) + sizeof(uint32_t));
        auto key = data.get<int64_t>();
        row.vector = data.take(vector_bytes);
        if (!fields) return;
        row.fields.resize(t.columns.size());
        for (size_t i = 0; i < t.columns.size(); ++i) {
            if (i == t.primary) row.fields[i] = key;
            else if (t.columns[i].type == Type::Bigint) row.fields[i] = data.get<int64_t>();
            else if (t.columns[i].type == Type::Varchar) row.fields[i] = data.string();
        }
    }
    static std::vector<Value> copy_row(const Table& t, const Row& row) {
        std::vector<Value> values;
        values.reserve(t.columns.size());
        for (size_t i = 0; i < t.columns.size(); ++i) {
            if (t.columns[i].type == Type::Vector) {
                std::vector<float> data(t.columns[i].size);
                std::memcpy(data.data(), row.vector.data(), row.vector.size());
                values.emplace_back(std::move(data));
            } else if (t.columns[i].type == Type::Bigint) values.emplace_back(std::get<int64_t>(row.fields[i]));
            else values.emplace_back(std::string(std::get<std::string_view>(row.fields[i])));
        }
        return values;
    }
    static std::vector<Filter> where(Parser& p, const Table& t) {
        std::vector<Filter> filters;
        if (!p.accept("where")) return filters;
        do {
            auto index = t.column(p.identifier());
            if (t.columns[index].type == Type::Vector) invalid("Vector cannot be compared directly");
            Compare op;
            if (p.accept("=")) op = Compare::Eq;
            else if (p.accept("<")) op = Compare::Lt;
            else if (p.accept("<=")) op = Compare::Le;
            else if (p.accept(">")) op = Compare::Gt;
            else if (p.accept(">=")) op = Compare::Ge;
            else syntax();
            filters.push_back({index, op, value(p, t.columns[index], false)});
        } while (p.accept("and"));
        return filters;
    }
    enum class Read { None, Vector, Fields };
    template <class Visitor>
    void matching(const Table& t, const std::vector<Filter>& filters, Read read, Visitor visitor) {
        auto vector_bytes = vector_size(t);
        Row row;
        auto visit = [&](int64_t key, uint64_t offset) {
            bool fields = read == Read::Fields || !filters.empty();
            if (fields || read == Read::Vector) decode(t, offset, vector_bytes, row, fields);
            for (const auto& filter : filters) if (!filter.match(row)) return true;
            return visitor(key, offset, row);
        };
        for (const auto& filter : filters) {
            if (filter.column == t.primary && filter.op == Compare::Eq) {
                auto key = std::get<int64_t>(filter.expected);
                auto offset = t.find(key);
                if (offset) visit(key, offset);
                return;
            }
        }
        t.visit(visit);
    }

    // Store only row references. The mapped record remains the source of vector bytes.
    void prepare_index(const Table& t) {
        if (index.table == &t) return;
        index = {};
        index.table = &t;
        index.entries.reserve(t.rows.size());
        for (const auto& [key, offset] : t.rows) index.entries.push_back({key, offset});
        std::sort(index.entries.begin(), index.entries.end(),
                  [](const IndexEntry& a, const IndexEntry& b) { return a.offset < b.offset; });
    }

    const std::vector<IndexEntry>* index_group(const Table& t, const Filter& filter) {
        static const std::vector<IndexEntry> empty;
        if (!index.filter_column) {
            auto column = filter.column;
            auto vector_bytes = vector_size(t);
            Row row;
            size_t text_bytes = 0;
            for (const auto& entry : index.entries) {
                decode(t, entry.offset, vector_bytes, row, true);
                if (auto key = std::get_if<int64_t>(&row.fields[column])) index.numeric[*key].push_back(entry);
                else {
                    auto key_text = std::get<std::string_view>(row.fields[column]);
                    auto existing = index.text.find(key_text);
                    if (existing == index.text.end()) {
                        text_bytes += key_text.size();
                        if (text_bytes > index_text_bytes) {
                            index.text.clear();
                            // This column is ineligible. Future requests fall back to scan.
                            index.filter_column = t.columns.size();
                            return nullptr;
                        }
                        existing = index.text.try_emplace(std::string(key_text)).first;
                    }
                    existing->second.push_back(entry);
                }
            }
            index.filter_column = column;
        }
        if (index.filter_column != filter.column) return nullptr;
        if (const auto* key = std::get_if<int64_t>(&filter.expected)) {
            auto found = index.numeric.find(*key);
            return found == index.numeric.end() ? &empty : &found->second;
        }
        auto found = index.text.find(std::get<std::string>(filter.expected));
        return found == index.text.end() ? &empty : &found->second;
    }

    Result create(Parser& p) {
        if (p.accept("vector")) {
            p.need("index");
            auto name = p.identifier();
            p.need("on");
            auto& t = table(p.identifier());
            p.need("(");
            auto column = t.column(p.identifier());
            p.need(")");
            p.finish();
            if (t.columns[column].type != Type::Vector) invalid("Index requires vector column");
            if (!t.index.empty()) throw Error("INDEX_EXISTS", "Vector index already exists");
            auto bytes = record_header(Op::Index, t.id);
            put_string(bytes, name);
            log.append(bytes);
            indexed.push_back(&t);
            t.index = std::move(name);
            return {};
        }
        p.need("table");
        auto name = p.identifier();
        if (tables.contains(name)) throw Error("TABLE_EXISTS", "Table already exists");
        p.need("(");
        std::vector<Column> columns;
        std::set<std::string> names;
        std::optional<std::string> primary;
        bool has_vector = false;
        auto set_primary = [&](std::string key) {
            if (primary) invalid("Expected one primary key");
            primary = std::move(key);
        };
        do {
            if (p.accept("primary")) {
                p.need("key");
                p.need("(");
                set_primary(p.identifier());
                p.need(")");
                continue;
            }
            Column column{p.identifier(), Type::Bigint, 0};
            if (!names.insert(column.name).second) invalid("Duplicate column name");
            if (p.accept("bigint")) {
                column.type = Type::Bigint;
            } else {
                if (p.accept("varchar")) column.type = Type::Varchar;
                else if (p.accept("vector")) {
                    if (has_vector) invalid("Expected at most one vector column");
                    has_vector = true;
                    column.type = Type::Vector;
                } else syntax();
                p.need("(");
                column.size = p.positive_size();
                p.need(")");
            }
            if (p.accept("primary")) {
                p.need("key");
                set_primary(column.name);
            }
            columns.push_back(std::move(column));
        } while (p.accept(","));
        p.need(")");
        p.finish();
        size_t primary_index = 0;
        while (primary && primary_index < columns.size() && columns[primary_index].name != *primary)
            ++primary_index;
        if (!primary || primary_index == columns.size() || columns[primary_index].type != Type::Bigint)
            invalid("Expected one BIGINT primary key");

        uint32_t id = static_cast<uint32_t>(ids.size());
        auto bytes = record_header(Op::Create, id);
        put_string(bytes, name);
        put(bytes, static_cast<uint32_t>(primary_index));
        put(bytes, static_cast<uint32_t>(columns.size()));
        for (const auto& column : columns) {
            put(bytes, column.type);
            put(bytes, column.size);
            put_string(bytes, column.name);
        }
        log.append(bytes);
        created.push_back(name);
        add_table(std::move(name), Table{id, std::move(columns), primary_index, {}, {}, {}});
        return {};
    }

    void publish(Table& t, std::map<int64_t, uint64_t>& staged) {
        if (staged.empty()) return;
        invalidate_index(t);
        changed.insert(&t);
        for (const auto& [key, offset] : staged) t.pending.erase(key);
        t.pending.merge(staged);
    }
    Result insert(Parser& p) {
        p.need("into");
        auto& t = table(p.identifier());
        std::vector<size_t> order;
        if (p.accept("(")) {
            std::set<size_t> seen;
            do {
                auto index = t.column(p.identifier());
                if (!seen.insert(index).second) invalid("Duplicate INSERT column");
                order.push_back(index);
            } while (p.accept(","));
            p.need(")");
            if (order.size() != t.columns.size()) invalid("All columns require values");
        } else {
            for (size_t i = 0; i < t.columns.size(); ++i) order.push_back(i);
        }
        p.need("values");
        std::map<int64_t, uint64_t> staged;
        std::vector<Value> values(order.size());
        do {
            p.need("(");
            for (size_t i = 0; i < order.size(); ++i) {
                if (i) p.need(",");
                values[order[i]] = value(p, t.columns[order[i]]);
            }
            p.need(")");
            auto key = std::get<int64_t>(values[t.primary]);
            if (t.find(key) || staged.contains(key)) throw Error("DUPLICATE_KEY", "Duplicate primary key");
            staged.emplace(key, log.append(encode(t, values)));
        } while (p.accept(","));
        p.finish();
        size_t count = staged.size();
        publish(t, staged);
        return {false, count, {}, 0, {}};
    }
    Result modify(Parser& p, bool deleting) {
        if (deleting) p.need("from");
        auto& t = table(p.identifier());
        std::map<size_t, Value> updates;
        if (!deleting) {
            p.need("set");
            do {
                auto column = t.column(p.identifier());
                if (column == t.primary) throw Error("UNSUPPORTED", "Primary key updates are unsupported");
                p.need("=");
                auto v = value(p, t.columns[column]);
                if (!updates.emplace(column, std::move(v)).second) invalid("Duplicate assignment");
            } while (p.accept(","));
        }
        auto filters = where(p, t);
        p.finish();
        std::map<int64_t, uint64_t> staged;
        matching(t, filters, deleting ? Read::None : Read::Fields, [&](int64_t key, uint64_t, const Row& row) {
            if (deleting) {
                auto bytes = record_header(Op::Delete, t.id);
                put(bytes, key);
                log.append(bytes);
                staged.emplace(key, 0);
            } else {
                auto values = copy_row(t, row);
                for (const auto& [column, v] : updates) values[column] = v;
                staged.emplace(key, log.append(encode(t, values)));
            }
            return true;
        });
        size_t count = staged.size();
        publish(t, staged);
        return {false, count, {}, 0, {}};
    }

    static Distance distance(Parser& p, const Table& t) {
        p.need("(");
        auto column = t.column(p.identifier());
        if (t.columns[column].type != Type::Vector) invalid("L2_DISTANCE requires vector column");
        p.need(",");
        auto target = p.vector(t.columns[column].size);
        p.need(")");
        return {column, std::move(target)};
    }
    static std::string real(double n) {
        char buffer[64];
#ifdef __APPLE__
        auto count = std::snprintf(buffer, sizeof buffer, "%.17g", n);
        return std::string(buffer, static_cast<size_t>(count));
#else
        auto converted = std::to_chars(buffer, buffer + sizeof buffer, n);
        return {buffer, converted.ptr};
#endif
    }
    static std::string display(const Table& t, const Row& row, const Expression& expression) {
        if (expression.distance) return real(std::sqrt(squared_distance(row.vector, expression.distance->target)));
        auto column = expression.column;
        if (t.columns[column].type == Type::Bigint) return std::to_string(std::get<int64_t>(row.fields[column]));
        if (t.columns[column].type == Type::Varchar) return std::string(std::get<std::string_view>(row.fields[column]));
        std::string result = "[";
        Reader bytes(row.vector);
        while (!bytes.empty()) {
            if (result.size() > 1) result += ',';
            float v = bytes.get<float>();
            char buffer[32];
#ifdef __APPLE__
            auto count = std::snprintf(buffer, sizeof buffer, "%.9g", static_cast<double>(v));
            result.append(buffer, static_cast<size_t>(count));
#else
            auto converted = std::to_chars(buffer, buffer + sizeof buffer, v);
            result.append(buffer, converted.ptr);
#endif
        }
        result += ']';
        return result;
    }

    Result select(Parser p) {
        // Resolve the table before parsing projection expressions and their types.
        Parser projection = p;
        unsigned depth = 0;
        while (p.kind() != Kind::End && (depth || !p.is("from"))) {
            if (p.accept("(")) ++depth;
            else if (p.accept(")")) {
                if (!depth) syntax();
                --depth;
            } else p.take();
        }
        p.need("from");
        const auto& t = table(p.identifier());

        std::vector<Expression> expressions;
        if (projection.accept("*")) {
            for (size_t i = 0; i < t.columns.size(); ++i)
                expressions.push_back({i, {}, t.columns[i].name});
        } else {
            do {
                Expression expression{};
                if (projection.accept("l2_distance")) {
                    expression.distance = distance(projection, t);
                    expression.label = "L2_DISTANCE";
                } else {
                    expression.column = t.column(projection.identifier());
                    expression.label = t.columns[expression.column].name;
                }
                if (projection.accept("as")) expression.label = projection.identifier();
                expressions.push_back(std::move(expression));
            } while (projection.accept(","));
        }
        projection.need("from");
        auto filters = where(p, t);
        std::optional<Expression> order;
        bool descending = false;
        if (p.accept("order")) {
            p.need("by");
            if (p.accept("l2_distance")) order = Expression{0, distance(p, t), {}};
            else {
                auto name = p.identifier();
                for (const auto& expr : expressions) {
                    if (expr.label == name) { order = expr; break; }
                }
                if (!order) order = Expression{t.column(name), {}, {}};
                if (!order->distance && t.columns[order->column].type == Type::Vector)
                    invalid("Vector cannot be sorted directly");
            }
            descending = p.accept("desc");
            if (!descending) p.accept("asc");
        }
        uint64_t limit = UINT64_MAX;
        if (p.accept("limit")) {
            auto n = p.integer();
            if (n < 0) invalid("Negative LIMIT");
            limit = n;
        }
        p.finish();

        Result result;
        result.query = true;
        for (const auto& expr : expressions) result.columns.push_back(expr.label);
        if (!limit) return result;

        auto vector_bytes = vector_size(t);
        bool need_fields = false;
        std::vector<bool> reuse_distance;
        for (const auto& expr : expressions) {
            need_fields |= !expr.distance && expr.column != t.primary &&
                           t.columns[expr.column].type != Type::Vector;
            reuse_distance.push_back(expr.distance && order && order->distance == expr.distance);
        }
        auto project = [this, &t, vector_bytes, need_fields, expressions = std::move(expressions),
                        reuse_distance = std::move(reuse_distance)]
                       (int64_t key, uint64_t offset, double squared, Row& row,
                        std::vector<std::string>& values) {
            decode(t, offset, vector_bytes, row, need_fields);
            for (size_t i = 0; i < expressions.size(); ++i) {
                const auto& expr = expressions[i];
                if (reuse_distance[i]) values[i] = real(std::sqrt(squared));
                else if (!expr.distance && expr.column == t.primary) values[i] = std::to_string(key);
                else values[i] = display(t, row, expr);
            }
        };

        bool ordered_by_primary = order && !order->distance && order->column == t.primary && !descending;
        size_t columns = result.columns.size();
        if (!order || ordered_by_primary) {
            size_t count = 0;
            if (filters.empty() && t.pending.empty()) count = std::min<uint64_t>(limit, t.rows.size());
            else matching(t, filters, Read::None,
                          [&](int64_t, uint64_t, const Row&) { return ++count < limit; });
            result.row_count = count;
            result.emit = [this, &t, filters = std::move(filters), count, columns,
                           project = std::move(project)](const Result::Emit& emit) {
                if (!count) return;
                Row row;
                std::vector<std::string> values(columns);
                size_t remaining = count;
                matching(t, filters, Read::None, [&](int64_t key, uint64_t offset, const Row&) {
                    project(key, offset, 0, row, values);
                    emit(values);
                    return --remaining > 0;
                });
            };
            return result;
        }

        struct Hit {
            int64_t key;
            uint64_t offset;
            Cell sort;
        };
        auto before = [&](const Hit& a, const Hit& b) {
            if (a.sort == b.sort) return a.key < b.key;
            return descending ? a.sort > b.sort : a.sort < b.sort;
        };
        std::vector<Hit> hits;
        bool sorted_column = !order->distance && order->column != t.primary;
        bool use_heap = limit != UINT64_MAX;
        auto consider = [&](Hit hit) {
            if (!use_heap) hits.push_back(hit);
            else if (hits.size() < limit) {
                hits.push_back(hit);
                std::push_heap(hits.begin(), hits.end(), before);
            } else if (before(hit, hits.front())) {
                std::pop_heap(hits.begin(), hits.end(), before);
                hits.back() = hit;
                std::push_heap(hits.begin(), hits.end(), before);
            }
        };
        const std::vector<IndexEntry>* candidates = nullptr;
        if (order && order->distance && !t.index.empty() && !transaction && t.rows.size() <= index_rows) {
            prepare_index(t);
            if (filters.empty()) candidates = &index.entries;
            else if (filters.size() == 1 && filters[0].op == Compare::Eq && filters[0].column != t.primary)
                candidates = index_group(t, filters[0]);
        }
        if (candidates && !candidates->empty()) {
            constexpr size_t vector_start = sizeof(uint32_t) + sizeof(Op) + sizeof(uint32_t) + sizeof(int64_t);
            const char* data = log.view(0, log.size()).data();
            const auto& target = order->distance->target;
            const auto calculate = distance_function();
            for (const auto& entry : *candidates) {
                double squared = calculate(data + entry.offset + vector_start, target.data(), target.size());
                consider({entry.key, entry.offset, squared});
            }
        } else if (!candidates) {
            Read read = sorted_column ? Read::Fields : order->distance ? Read::Vector : Read::None;
            matching(t, filters, read, [&](int64_t key, uint64_t offset, const Row& row) {
                Hit hit{key, offset, key};
                if (order->distance) hit.sort = squared_distance(row.vector, order->distance->target);
                else if (sorted_column) hit.sort = row.fields[order->column];
                consider(hit);
                return true;
            });
        }
        std::sort(hits.begin(), hits.end(), before);
        result.row_count = hits.size();
        bool distance_order = order->distance.has_value();
        result.emit = [hits = std::move(hits), columns, distance_order,
                       project = std::move(project)](const Result::Emit& emit) {
            Row row;
            std::vector<std::string> values(columns);
            for (const auto& hit : hits) {
                project(hit.key, hit.offset, distance_order ? std::get<double>(hit.sort) : 0,
                        row, values);
                emit(values);
            }
        };
        return result;
    }

public:
    explicit Database(const std::string& dir) : log(dir) {
        log.recover([&](uint64_t offset, std::string_view bytes) { recover(offset, bytes); });
    }
    void close() {
        if (transaction) { rollback(); transaction = false; }
    }
    Result execute(std::string_view sql) {
        Parser p(sql);
        if (p.accept("begin")) {
            p.finish();
            if (transaction) throw Error("TRANSACTION_ERROR", "Transaction already open");
            log.begin();
            transaction = true;
            return {};
        }
        if (p.is("commit") || p.is("rollback")) {
            bool committing = p.accept("commit");
            if (!committing) p.need("rollback");
            p.finish();
            if (!transaction) throw Error("TRANSACTION_ERROR", "No active transaction");
            if (committing) commit();
            else rollback();
            transaction = false;
            return {};
        }
        if (p.accept("select")) return select(p);
        bool creating = p.accept("create");
        bool inserting = !creating && p.accept("insert");
        bool updating = !creating && !inserting && p.accept("update");
        bool deleting = !creating && !inserting && !updating && p.accept("delete");
        if (!creating && !inserting && !updating && !deleting)
            throw Error("UNSUPPORTED", "Unsupported statement");
        if (!transaction) log.begin();
        auto checkpoint = log.size();
        try {
            auto result = creating ? create(p) : inserting ? insert(p) : modify(p, deleting);
            if (!transaction) commit();
            return result;
        } catch (const Error&) {
            if (transaction) log.rewind(checkpoint);
            else rollback();
            throw;
        }
    }
};
} // namespace mini
