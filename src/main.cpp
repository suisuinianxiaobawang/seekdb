#include "database.hpp"
#include <cstdlib>
#include <iostream>

namespace {
void write_field(std::ostream& out, std::string_view field) {
    for (char c : field) {
        switch (c) {
            case '\0': out << "\\0"; break;
            case '\t': out << "\\t"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\\': out << "\\\\"; break;
            default: out.put(c);
        }
    }
}
void write_row(const std::vector<std::string>& fields) {
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) std::cout.put('\t');
        write_field(std::cout, fields[i]);
    }
    std::cout.put('\n');
}

class Session {
    mini::Database db;
    std::string statement;
    char quote = 0;
    bool block_comment = false;
    bool running = true;

    void execute() {
        try {
            if (mini::Parser(statement).kind() == mini::Kind::End) return;
            auto result = db.execute(statement);
            if (result.query) {
                std::cout << "RESULT\t" << result.row_count << '\t' << result.columns.size() << '\n';
                write_row(result.columns);
                if (result.emit) result.emit(write_row);
            } else std::cout << "OK\t" << result.affected << '\n';
        } catch (const mini::Error& error) {
            std::cout << "ERROR\t" << error.code << '\t';
            write_field(std::cout, error.what());
            std::cout.put('\n');
        }
        std::cout.flush();
    }

public:
    explicit Session(const std::string& dir) : db(dir) {}
    void read_line(std::string_view line) {
        if (statement.empty() && !block_comment) {
            auto first = line.find_first_not_of(" \t\r");
            if (first == line.npos) return;
            line.remove_prefix(first);
        }
        if (statement.empty() && !block_comment && line.size() < 32) {
            auto control = mini::lower(line);
            while (!control.empty() && mini::space(control.back())) control.pop_back();
            if (control == ".quit" || control == "quit" || control == "exit" || control == "quit;") {
                running = false;
                return;
            }
            if (control == ".mode exact" || control == ".mode ann") {
                std::cout << "OK\t0\n" << std::flush;
                return;
            }
        }
        if (statement.empty()) statement.reserve(line.size());
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            char next = i + 1 < line.size() ? line[i + 1] : '\0';
            if (block_comment) {
                if (c == '*' && next == '/') { block_comment = false; ++i; }
            } else if (quote) {
                statement += c;
                if (c == '\\' && quote != '`' && i + 1 < line.size()) statement += line[++i];
                else if (c == quote) {
                    if (next == quote) statement += line[++i];
                    else quote = 0;
                }
            } else if (c == '#' || (c == '-' && next == '-')) {
                break;
            } else if (c == '/' && next == '*') {
                block_comment = true;
                ++i;
                if (!statement.empty()) statement += ' ';
            } else if (c == ';') {
                execute();
                statement.clear();
            } else {
                if (c == '\'' || c == '"' || c == '`') quote = c;
                if (!statement.empty() || !mini::space(c)) statement += c;
            }
        }
        if (!statement.empty()) statement += '\n';
    }
    void run() {
        std::cout << "READY\n" << std::flush;
        std::string line;
        while (running && std::getline(std::cin, line)) read_line(line);
        if (running && (!statement.empty() || block_comment)) {
            if (block_comment) statement += "/*";
            execute();
        }
        db.close();
    }
};
} // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    try {
        std::string directory = "/data";
        if (argc == 3 && std::string_view(argv[1]) == "--data-dir") directory = argv[2];
        else if (argc != 1) {
            std::cerr << "Usage: minivectordb [--data-dir PATH]\n";
            return 2;
        }
        Session(directory).run();
    } catch (const std::exception& error) {
        std::cerr << "minivectordb: " << error.what() << '\n';
        return 1;
    }
}
