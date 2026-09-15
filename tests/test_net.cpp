// Loopback connections (overall design 6.3, experiment X2): what the workspace daemon listens on.
import std;
import mcppls.testing;
import mcppls.platform.net;

namespace net = mcppls::platform::net;

int main() {
    using namespace mcppls::testing;

    "a loopback connection carries bytes both ways and ends when one side is done"_test = [] {
        auto listener = net::Listener::listen_local();
        expect(fatal(listener.has_value())) << (listener ? std::string {} : listener.error().message);
        expect(listener->port() > 0);
        std::optional<std::string> received;
        std::thread server { [&] {
            auto accepted = listener->accept();
            if (!accepted) return;
            std::string all;
            while (true) {
                auto chunk = accepted->read();
                if (!chunk || chunk->empty()) break;
                all += *chunk;
            }
            received = all;
            (void)accepted->write("pong\n");
            accepted->close();
        } };
        auto client = net::Connection::connect_local(listener->port());
        expect(fatal(client.has_value())) << (client ? std::string {} : client.error().message);
        expect(client->write("ping\n").has_value());
        // Half-closed: the server reads the end of the stream and still answers.
        client->shutdown_write();
        std::string answer;
        while (true) {
            auto chunk = client->read();
            if (!chunk || chunk->empty()) break;
            answer += *chunk;
        }
        server.join();
        expect(received == std::optional<std::string> { "ping\n" });
        expect(answer == "pong\n") << answer;
    };

    "tokens are random hex"_test = [] {
        const std::string first { net::random_token() };
        const std::string second { net::random_token() };
        expect(first.size() == 32u && second.size() == 32u);
        expect(first != second);
        expect(std::ranges::all_of(first, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    };

    "connecting where nothing listens is an error"_test = [] {
        int port { 0 };
        {
            auto listener = net::Listener::listen_local();
            expect(fatal(listener.has_value()));
            port = listener->port();
        }
        expect(!net::Connection::connect_local(port).has_value());
    };

    return report();
}
