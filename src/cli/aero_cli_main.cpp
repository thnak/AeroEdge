// AeroEdge CLI (spec 013 §5 aero-cli).
//
// The headless equivalent of the Studio for CI/CD and scripting — same API, over httplib::Client:
//   aero deploy   <app.json> [--url http://host:port]
//   aero reload   <app.json> [--url http://host:port]   # hot-reload the running app (009 §4)
//   aero rollback <name>     [--url http://host:port]   # roll back to the prior version (009 §6)
//   aero status              [--url http://host:port]
//   aero undeploy <name>     [--url http://host:port]
// Prints the server's HTTP status + JSON body. Exit 0 on a 2xx response, non-zero otherwise.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int report(const httplib::Result& res) {
    if (!res) {
        std::fprintf(stderr, "request failed: %s\n", httplib::to_string(res.error()).c_str());
        return 1;
    }
    std::printf("HTTP %d\n%s\n", res->status, res->body.c_str());
    return (res->status >= 200 && res->status < 300) ? 0 : 1;
}

void usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  aero deploy   <app.json> [--url URL]\n"
                 "  aero reload   <app.json> [--url URL]\n"
                 "  aero rollback <name>     [--url URL]\n"
                 "  aero status              [--url URL]\n"
                 "  aero undeploy <name>     [--url URL]\n"
                 "  (default URL: http://127.0.0.1:8080)\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string url = "http://127.0.0.1:8080";
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--url" && i + 1 < argc) {
            url = argv[++i];
        } else {
            pos.push_back(a);
        }
    }

    if (pos.empty()) {
        usage();
        return 2;
    }

    httplib::Client cli(url);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(10, 0);

    const std::string& cmd = pos[0];
    if (cmd == "deploy") {
        if (pos.size() < 2) {
            usage();
            return 2;
        }
        const std::string body = read_file(pos[1]);
        if (body.empty()) {
            std::fprintf(stderr, "cannot read '%s'\n", pos[1].c_str());
            return 2;
        }
        return report(cli.Post("/apps", body, "application/json"));
    }
    if (cmd == "reload") {
        if (pos.size() < 2) {
            usage();
            return 2;
        }
        const std::string body = read_file(pos[1]);
        if (body.empty()) {
            std::fprintf(stderr, "cannot read '%s'\n", pos[1].c_str());
            return 2;
        }
        // Reload is PUT /apps/{name}; derive {name} from the Application JSON body.
        auto j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.contains("name") || !j["name"].is_string()) {
            std::fprintf(stderr, "'%s' is not a valid Application (missing string 'name')\n",
                         pos[1].c_str());
            return 2;
        }
        return report(cli.Put("/apps/" + j["name"].get<std::string>(), body, "application/json"));
    }
    if (cmd == "rollback") {
        if (pos.size() < 2) {
            usage();
            return 2;
        }
        return report(cli.Post("/apps/" + pos[1] + "/rollback", "", "application/json"));
    }
    if (cmd == "status") {
        return report(cli.Get("/status"));
    }
    if (cmd == "undeploy") {
        if (pos.size() < 2) {
            usage();
            return 2;
        }
        return report(cli.Delete("/apps/" + pos[1]));
    }

    usage();
    return 2;
}
