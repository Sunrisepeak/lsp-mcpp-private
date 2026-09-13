import std;
import lspmcpp.testing;
import lspmcpp.platform.task;

int main() {
    using namespace lspmcpp::testing;
    using lspmcpp::platform::Channel;

    "values cross threads in order"_test = [] {
        Channel<int> channel;
        std::jthread producer { [&] {
            for (int i { 0 }; i < 1000; ++i) channel.push(i);
            channel.close();
        } };
        int expected { 0 };
        bool ordered { true };
        while (auto value = channel.pop()) ordered = ordered && *value == expected++;
        expect(ordered);
        expect(expected == 1000_i);
    };

    "a deadline expires without a value"_test = [] {
        Channel<std::string> channel;
        const auto started = std::chrono::steady_clock::now();
        auto value = channel.pop_until(started + std::chrono::milliseconds { 50 });
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(!value.has_value());
        expect(elapsed >= std::chrono::milliseconds { 40 });
        expect(elapsed < std::chrono::seconds { 10 });
    };

    "closing wakes a waiting reader"_test = [] {
        Channel<int> channel;
        std::jthread closer { [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
            channel.close();
        } };
        expect(!channel.pop().has_value());
        expect(!channel.push(1));
    };

    return report();
}
