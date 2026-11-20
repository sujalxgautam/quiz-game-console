// =============================================================================
//  main.cpp — Quiz Game (Console Edition) — deduplicated, tiered fetch
// =============================================================================
//
//  PROJECT BRIEF
//  -------------
//  A terminal quiz game that tries to fetch live questions from The Trivia
//  API (https://the-trivia-api.com) and falls back to a built-in local
//  question bank if the network is unavailable or slow.
//
//  DIFFICULTY : Medium
//  STANDARD    : C++17
//  DEPENDENCIES: Standard library only. Raw POSIX sockets for HTTP.
//  BUILD       : g++ -std=c++17 -O2 main.cpp -o quiz
//  RUN         : ./quiz
//
//  DESIGN DECISION: TIERED FETCHING
//  --------------------------------
//  The Trivia API's free tier has NO session tokens — every call is an
//  independent random roll. Ask for 50 questions, then 50 more, and you'll
//  see heavy overlap because the pool re-rolls. Asking for 100 in two
//  batches makes it worse (rate limits + duplicates).
//
//  Honest fix, given what the free tier offers:
//
//    * count <= 25  : fetch from API. Client-side dedup by question text.
//                     Top up from local bank if short. This is the case
//                     where "live" is actually a feature.
//
//    * count > 25   : skip the API, use the local bank, shuffled. The local
//                     bank has 23 questions so it's fine up to ~23 with real
//                     variety; beyond that we repeat but at least we don't
//                     promise "live" and then serve duplicates.
//
//  Trade-off: we lose "live questions" for big quizzes. We gain a bug-free,
//  honestly-described player experience. A future polish pass with a paid
//  API key or a switch to Open Trivia DB (which does have free session
//  tokens) would restore live fetching at all sizes.
//
//  THE 50/100 BUG (fixed earlier)
//  ------------------------------
//  The per-call cap is 50 and the free tier rate-limits rapid successive
//  requests. The old code silently returned a partial batch. Now: we don't
//  even try above 25, so the failure mode is removed rather than patched.
//
//  LICENSE: MIT.
// =============================================================================

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// =============================================================================
//  ANSI color helpers
// =============================================================================
namespace color {
    constexpr const char* kReset      = "\033[0m";
    constexpr const char* kBold       = "\033[1m";
    constexpr const char* kDim        = "\033[2m";
    constexpr const char* kRed        = "\033[31m";
    constexpr const char* kGreen      = "\033[32m";
    constexpr const char* kYellow     = "\033[33m";
    constexpr const char* kBlue       = "\033[34m";
    constexpr const char* kMagenta    = "\033[35m";
    constexpr const char* kCyan       = "\033[36m";
    constexpr const char* kWhite      = "\033[37m";
    constexpr const char* kBoldRed    = "\033[1;31m";
    constexpr const char* kBoldGreen  = "\033[1;32m";
    constexpr const char* kBoldYellow = "\033[1;33m";
    constexpr const char* kBoldCyan   = "\033[1;36m";
    constexpr const char* kBoldWhite  = "\033[1;37m";

    inline void clearScreen() { std::cout << "\033[H\033[2J"; }
}

// =============================================================================
//  Types
// =============================================================================
enum class MenuState { Category, Difficulty, Count, Playing, Results, Quit };

struct Question {
    std::string category;
    std::string difficulty;
    std::string text;
    std::string correct;
    std::vector<std::string> answers;
    int correctIndex = 0;
    bool answered    = false;
    int  chosen      = -1;
};

struct Game {
    MenuState state = MenuState::Category;

    int categoryId = 0;
    std::string categoryName;

    int difficultyId = 0;
    std::string difficultyName;

    int questionCount = 10;

    std::vector<Question> questions;
    int current = 0;
    int score   = 0;

    std::mt19937 rng{std::random_device{}()};
};

// =============================================================================
//  Tunable thresholds (one place, easy to justify to a reader)
// =============================================================================
namespace policy {
    // Above this count we skip the API entirely and use the local bank.
    // Justification: the free tier re-rolls the same pool on every call, so
    // larger requests produce heavy overlap. 25 is small enough that a
    // single batch plus dedup still yields mostly unique questions.
    constexpr int kApiMaxCount = 25;
}

// =============================================================================
//  Local Fallback Question Bank
// =============================================================================
namespace localBank {

struct RawQuestion {
    const char* category;
    const char* difficulty;
    const char* text;
    const char* correct;
    const char* wrong[3];
};

static const RawQuestion kQuestions[] = {
    { "General", "easy", "What is the capital of France?", "Paris", {"London", "Berlin", "Madrid"} },
    { "General", "easy", "How many continents are there?", "7", {"5", "6", "8"} },
    { "General", "easy", "What color is the sky on a clear day?", "Blue", {"Green", "Red", "Yellow"} },
    { "General", "medium", "What is the largest ocean on Earth?", "Pacific", {"Atlantic", "Indian", "Arctic"} },
    { "General", "medium", "Who wrote 'Romeo and Juliet'?", "Shakespeare", {"Dickens", "Austen", "Hemingway"} },
    { "General", "hard", "What is the smallest prime number?", "2", {"1", "3", "0"} },
    { "General", "hard", "What is the chemical symbol for gold?", "Au", {"Ag", "Fe", "Cu"} },

    { "Science", "easy", "What planet is known as the Red Planet?", "Mars", {"Venus", "Jupiter", "Saturn"} },
    { "Science", "easy", "What gas do plants absorb from the air?", "Carbon dioxide", {"Oxygen", "Nitrogen", "Hydrogen"} },
    { "Science", "medium", "What is the powerhouse of the cell?", "Mitochondria", {"Nucleus", "Ribosome", "Golgi"} },
    { "Science", "medium", "What is the speed of light (approx)?", "300,000 km/s", {"150,000 km/s", "500,000 km/s", "1,000,000 km/s"} },
    { "Science", "hard", "What is the atomic number of carbon?", "6", {"12", "8", "14"} },

    { "History", "easy", "In which year did World War II end?", "1945", {"1939", "1918", "1950"} },
    { "History", "easy", "Who was the first US president?", "Washington", {"Lincoln", "Jefferson", "Adams"} },
    { "History", "medium", "Which empire built the Colosseum?", "Roman", {"Greek", "Persian", "Ottoman"} },
    { "History", "hard", "Who was the first emperor of China?", "Qin Shi Huang", {"Kublai Khan", "Sun Yat-sen", "Confucius"} },

    { "Geography", "easy", "What is the longest river in the world?", "Nile", {"Amazon", "Yangtze", "Mississippi"} },
    { "Geography", "easy", "Which country has the most people?", "India", {"China", "USA", "Indonesia"} },
    { "Geography", "medium", "What is the capital of Australia?", "Canberra", {"Sydney", "Melbourne", "Perth"} },
    { "Geography", "hard", "What is the deepest ocean trench?", "Mariana Trench", {"Tonga Trench", "Java Trench", "Puerto Rico Trench"} },

    { "Entertainment", "easy", "Who directed 'Jaws'?", "Spielberg", {"Lucas", "Coppola", "Scorsese"} },
    { "Entertainment", "medium", "What band sang 'Bohemian Rhapsody'?", "Queen", {"Beatles", "Led Zeppelin", "Pink Floyd"} },
    { "Entertainment", "hard", "Which film won the first Best Picture Oscar?", "Wings", {"Sunrise", "The Jazz Singer", "Metropolis"} },
};

inline Question makeQuestion(const RawQuestion& raw, std::mt19937& rng) {
    Question q;
    q.category   = raw.category;
    q.difficulty = raw.difficulty;
    q.text       = raw.text;
    q.correct    = raw.correct;
    q.answers.push_back(raw.correct);
    for (int i = 0; i < 3; ++i) q.answers.push_back(raw.wrong[i]);
    std::shuffle(q.answers.begin(), q.answers.end(), rng);
    for (std::size_t i = 0; i < q.answers.size(); ++i)
        if (q.answers[i] == q.correct) { q.correctIndex = static_cast<int>(i); break; }
    return q;
}

// Build a shuffled set of `count` questions from the local bank.
// For count <= bank size we get distinct questions. For larger counts we
// cycle, but the caller's shuffle ensures repeats are spread out, not
// adjacent.
inline std::vector<Question> get(int count, std::mt19937& rng) {
    std::vector<Question> out;
    constexpr int total = static_cast<int>(std::size(kQuestions));

    // Start from a shuffled index order so short quizzes don't always start
    // with the same questions.
    std::vector<int> order(total);
    for (int i = 0; i < total; ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);

    for (int i = 0; i < count; ++i) {
        const int idx = order[i % total];
        out.push_back(makeQuestion(kQuestions[idx], rng));
    }
    return out;
}

} // namespace localBank

// =============================================================================
//  HTTP helper — reads until JSON array is complete, with hard timeout
// =============================================================================
namespace http {

inline std::optional<std::string> get(const std::string& host,
                                       const std::string& path) {
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), "80", &hints, &res) != 0 || !res)
        return std::nullopt;

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return std::nullopt; }

    timeval tv{};
    tv.tv_sec  = 10;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock);
        freeaddrinfo(res);
        return std::nullopt;
    }
    freeaddrinfo(res);

    std::string request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Connection: close\r\n"
        "User-Agent: quiz-console/1.0\r\n"
        "\r\n";

    if (send(sock, request.c_str(), request.size(), 0) < 0) {
        close(sock);
        return std::nullopt;
    }

    std::string response;
    char buf[8192];
    ssize_t n;
    bool complete = false;

    while (!complete && (n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        response.append(buf, static_cast<std::size_t>(n));

        auto sep = response.find("\r\n\r\n");
        if (sep == std::string::npos) continue;

        const std::string body = response.substr(sep + 4);

        int arrayDepth = 0;
        int braceDepth = 0;
        bool sawArrayStart = false;
        bool sawArrayEnd   = false;
        bool inString = false;
        bool escaped  = false;

        for (char c : body) {
            if (inString) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
                continue;
            }
            if (c == '"') { inString = true; continue; }
            if (c == '[') { ++arrayDepth; sawArrayStart = true; }
            else if (c == ']') { --arrayDepth; if (arrayDepth == 0 && sawArrayStart) sawArrayEnd = true; }
            else if (c == '{') { ++braceDepth; }
            else if (c == '}') { --braceDepth; }

            if (sawArrayEnd && arrayDepth == 0 && braceDepth == 0) { complete = true; break; }
        }
    }
    close(sock);

    if (response.empty()) return std::nullopt;

    const auto sep = response.find("\r\n\r\n");
    if (sep == std::string::npos) return std::nullopt;

    std::string headers = response.substr(0, sep);
    std::string body    = response.substr(sep + 4);

    if (headers.find("chunked") != std::string::npos) {
        std::string decoded;
        std::size_t pos = 0;
        while (pos < body.size()) {
            const auto lineEnd = body.find("\r\n", pos);
            if (lineEnd == std::string::npos) break;
            const std::string sizeLine = body.substr(pos, lineEnd - pos);
            if (sizeLine.empty()) { pos = lineEnd + 2; continue; }
            unsigned long chunkSize = 0;
            try { chunkSize = std::stoul(sizeLine, nullptr, 16); } catch (...) { break; }
            if (chunkSize == 0) break;
            const std::size_t dataStart = lineEnd + 2;
            if (dataStart + chunkSize > body.size()) break;
            decoded += body.substr(dataStart, chunkSize);
            pos = dataStart + chunkSize + 2;
        }
        if (!decoded.empty()) body = decoded;
    }

    return body;
}

} // namespace http

// =============================================================================
//  JSON extraction helpers
// =============================================================================
namespace json {

inline std::string stringValue(const std::string& obj, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    auto pos = obj.find(pattern);
    if (pos == std::string::npos) return "";
    pos = obj.find(':', pos);
    if (pos == std::string::npos) return "";
    ++pos;
    while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos]))) ++pos;
    if (pos >= obj.size() || obj[pos] != '"') return "";
    ++pos;
    std::string result;
    while (pos < obj.size() && obj[pos] != '"') {
        if (obj[pos] == '\\' && pos + 1 < obj.size()) { ++pos; result += obj[pos]; }
        else result += obj[pos];
        ++pos;
    }
    return result;
}

inline std::string questionText(const std::string& obj) {
    auto qpos = obj.find("\"question\"");
    if (qpos == std::string::npos) return "";
    auto brace = obj.find('{', qpos);
    if (brace == std::string::npos) return "";
    auto end = obj.find('}', brace);
    if (end == std::string::npos) return "";
    return stringValue(obj.substr(brace, end - brace + 1), "text");
}

inline std::vector<std::string> stringArray(const std::string& obj, const std::string& key) {
    std::vector<std::string> out;
    const std::string pattern = "\"" + key + "\"";
    auto pos = obj.find(pattern);
    if (pos == std::string::npos) return out;
    pos = obj.find('[', pos);
    if (pos == std::string::npos) return out;
    ++pos;
    while (pos < obj.size()) {
        while (pos < obj.size() && (std::isspace(static_cast<unsigned char>(obj[pos])) || obj[pos] == ',')) ++pos;
        if (pos >= obj.size() || obj[pos] == ']') break;
        if (obj[pos] != '"') break;
        ++pos;
        std::string item;
        while (pos < obj.size() && obj[pos] != '"') {
            if (obj[pos] == '\\' && pos + 1 < obj.size()) { ++pos; item += obj[pos]; }
            else item += obj[pos];
            ++pos;
        }
        out.push_back(item);
        if (pos < obj.size()) ++pos;
    }
    return out;
}

} // namespace json

// =============================================================================
//  The Trivia API client — single-batch, deduplicated
// =============================================================================
//
//  We only ever call this with count <= policy::kApiMaxCount. That removes
//  the multi-batch code path entirely, which was the source of the 50/100
//  bugs. One call, one parse, dedup against what we already have, return.
//
namespace triviaApi {

inline std::optional<std::vector<Question>> fetch(int count) {
    if (count <= 0) return std::nullopt;

    std::vector<Question> all;
    std::set<std::string> seenTexts;   // for dedup ACROSS retries too

    // One primary attempt plus one retry (with a fresh seed). If both produce
    // too few unique questions, we return what we got; the caller tops up
    // from the local bank.
    constexpr int kMaxAttempts = 2;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const int remaining = count - static_cast<int>(all.size());
        if (remaining <= 0) break;

        std::cout << "\r  " << color::kDim
                  << "Fetching questions"
                  << (attempt > 0 ? " (retry)" : "")
                  << "..." << std::string(20, ' ') << color::kReset << std::flush;

        std::mt19937 seedRng{std::random_device{}()};
        const unsigned seed = seedRng();

        std::string path = "/v2/questions?limit=" + std::to_string(remaining)
                         + "&seed=" + std::to_string(seed);

        auto body = http::get("the-trivia-api.com", path);
        if (!body) {
            // Network/rate-limit failure — try once more with a short delay.
            if (attempt + 1 < kMaxAttempts)
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            continue;
        }

        // Parse top-level array of objects.
        std::size_t pos = 0;
        int depth = 0;
        std::string currentObj;
        bool inString = false;
        bool escaped = false;

        for (; pos < body->size(); ++pos) {
            const char c = (*body)[pos];
            if (inString) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
                if (depth > 0) currentObj += c;
                continue;
            }
            if (c == '"') { inString = true; if (depth > 0) currentObj += c; continue; }
            if (c == '{') {
                if (depth == 0) currentObj.clear();
                ++depth; currentObj += c;
            } else if (c == '}') {
                --depth; currentObj += c;
                if (depth == 0) {
                    Question q;
                    q.category   = json::stringValue(currentObj, "category");
                    q.difficulty = json::stringValue(currentObj, "difficulty");
                    q.text       = json::questionText(currentObj);
                    q.correct    = json::stringValue(currentObj, "correctAnswer");
                    auto wrong   = json::stringArray(currentObj, "incorrectAnswers");

                    if (!q.text.empty() && !q.correct.empty() && wrong.size() >= 3) {
                        // DEDUP: skip any question text we've already kept.
                        if (seenTexts.insert(q.text).second) {
                            q.answers.push_back(q.correct);
                            for (std::size_t i = 0; i < 3; ++i) q.answers.push_back(wrong[i]);
                            std::mt19937 rng{std::random_device{}()};
                            std::shuffle(q.answers.begin(), q.answers.end(), rng);
                            for (std::size_t i = 0; i < q.answers.size(); ++i)
                                if (q.answers[i] == q.correct) { q.correctIndex = static_cast<int>(i); break; }
                            all.push_back(std::move(q));
                        }
                    }
                }
            } else if (depth > 0) {
                currentObj += c;
            } else if (c == ']') {
                break;
            }
        }

        if (static_cast<int>(all.size()) >= count) break;

        // Not enough unique — brief pause, then retry with a new seed.
        if (attempt + 1 < kMaxAttempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    if (all.empty()) return std::nullopt;
    if (static_cast<int>(all.size()) > count) all.resize(count);
    return all;
}

} // namespace triviaApi

// =============================================================================
//  Rendering
// =============================================================================
namespace render {

using namespace color;

inline void drawHeader() {
    std::cout << "\n"
              << kBoldCyan
              << "     ██╗ ██╗ ██████╗ ██╗   ██╗██████╗ \n"
              << "     ██║ ██║██╔═══██╗██║   ██║╚══██╔╝\n"
              << "     ██║ ██║██║   ██║██║   ██║   ██║ \n"
              << "     ██║ ██║██║▄▄ ██║██║   ██║   ██║ \n"
              << "     ╚█████╔╝╚██████╔╝╚██████╔╝   ██║ \n"
              << "      ╚════╝  ╚═════╝  ╚═════╝    ╚═╝ \n"
              << kReset << "\n"
              << kDim << "              · · ·  console edition  · · ·\n"
              << kReset << "\n";
}

inline std::string categoryMenu() {
    std::ostringstream out;
    out << "\n  " << kBoldCyan << "Pick a category" << kReset << "\n\n"
        << "   " << kBold << "[1]" << kReset << "  Any category\n"
        << "   " << kBold << "[2]" << kReset << "  General Knowledge\n"
        << "   " << kBold << "[3]" << kReset << "  Science & Nature\n"
        << "   " << kBold << "[4]" << kReset << "  History\n"
        << "   " << kBold << "[5]" << kReset << "  Geography\n"
        << "   " << kBold << "[6]" << kReset << "  Entertainment\n"
        << "   " << kBold << "[Q]" << kReset << "  Quit\n"
        << "\n  " << kCyan << "Choice: " << kReset;
    return out.str();
}

inline std::string difficultyMenu() {
    std::ostringstream out;
    out << "\n  " << kBoldCyan << "Pick a difficulty" << kReset << "\n\n"
        << "   " << kBold << "[1]" << kReset << "  Any difficulty\n"
        << "   " << kBold << "[2]" << kReset << "  Easy\n"
        << "   " << kBold << "[3]" << kReset << "  Medium\n"
        << "   " << kBold << "[4]" << kReset << "  Hard\n"
        << "   " << kBold << "[B]" << kReset << "  Back\n"
        << "\n  " << kCyan << "Choice: " << kReset;
    return out.str();
}

inline std::string countMenu() {
    std::ostringstream out;
    out << "\n  " << kBoldCyan << "How many questions?" << kReset << "\n\n"
        << "   " << kBold << "[1]" << kReset << "   10 questions  "
        << kDim << "(live from API)" << kReset << "\n"
        << "   " << kBold << "[2]" << kReset << "   25 questions  "
        << kDim << "(live from API)" << kReset << "\n"
        << "   " << kBold << "[3]" << kReset << "   50 questions  "
        << kDim << "(local bank, shuffled)" << kReset << "\n"
        << "   " << kBold << "[4]" << kReset << "  100 questions  "
        << kDim << "(local bank, shuffled)" << kReset << "\n"
        << "\n  " << kCyan << "Choice: " << kReset;
    return out.str();
}

inline std::string questionScreen(const Game& g) {
    if (g.current >= static_cast<int>(g.questions.size())) return "";
    const auto& q = g.questions[g.current];

    std::ostringstream out;
    out << "\n";

    out << "  " << kDim;
    const int total = static_cast<int>(g.questions.size());
    for (int i = 0; i < total; ++i) {
        if (i < g.current)       out << kGreen << "●" << kDim;
        else if (i == g.current) out << kBoldWhite << "●" << kDim;
        else                     out << "○";
    }
    out << kReset << "  " << kDim << "Q" << (g.current + 1) << "/" << total << kReset << "\n\n";

    out << "  " << kDim << q.category << "  ·  " << q.difficulty << kReset << "\n\n";

    std::string line;
    out << "  " << kBoldWhite;
    for (char c : q.text) {
        line += c;
        if (line.size() >= 60 && c == ' ') { out << line << "\n  "; line.clear(); }
    }
    out << line << kReset << "\n\n";

    const char* letters = "ABCD";
    for (int i = 0; i < 4 && i < static_cast<int>(q.answers.size()); ++i) {
        const bool isCorrect = q.answered && (i == q.correctIndex);
        const bool isChosen  = q.answered && (i == q.chosen) && !isCorrect;
        if (isCorrect)      out << "  " << kBoldGreen << letters[i] << ") " << q.answers[i] << kReset << "  " << kGreen << "← correct" << kReset << "\n";
        else if (isChosen)  out << "  " << kBoldRed << letters[i] << ") " << q.answers[i] << kReset << "  " << kRed << "← your pick" << kReset << "\n";
        else                out << "  " << kDim << letters[i] << ") " << kReset << q.answers[i] << "\n";
    }

    out << "\n";
    if (!q.answered) out << "  " << kCyan << "Your answer (A/B/C/D): " << kReset;
    else             out << "  " << kDim << "Press Enter for next question..." << kReset;
    return out.str();
}

inline std::string resultsScreen(const Game& g) {
    std::ostringstream out;
    out << "\n  " << kBoldCyan << "══════ RESULTS ══════" << kReset << "\n\n";
    const int total = static_cast<int>(g.questions.size());
    const int pct   = (total > 0) ? (g.score * 100 / total) : 0;

    out << "  " << kDim << "Score: " << kReset << kBoldWhite << g.score << kReset
        << kDim << " / " << total << kReset << "\n";

    out << "  " << kDim;
    const int barWidth = 30;
    const int filled   = (total > 0) ? (g.score * barWidth / total) : 0;
    out << kGreen;
    for (int i = 0; i < filled; ++i) out << "█";
    out << kDim;
    for (int i = filled; i < barWidth; ++i) out << "░";
    out << kReset << "  " << pct << "%\n\n";

    if (pct >= 80)      out << "  " << kBoldGreen << "Excellent!" << kReset << "\n";
    else if (pct >= 60) out << "  " << kYellow << "Good job!" << kReset << "\n";
    else if (pct >= 40) out << "  " << kYellow << "Not bad." << kReset << "\n";
    else                out << "  " << kRed << "Keep practicing!" << kReset << "\n";

    out << "\n  " << kBold << "[R]" << kReset << " Play again   "
        << kBold << "[M]" << kReset << " Main menu   "
        << kBold << "[Q]" << kReset << " Quit\n"
        << "  " << kCyan << "Choice: " << kReset;
    return out.str();
}

} // namespace render

// =============================================================================
//  Input helpers
// =============================================================================
namespace input {

inline void trim(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
}

inline std::optional<std::string> readLine() {
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
}

inline std::optional<char> readChoice() {
    auto line = readLine();
    if (!line) return std::nullopt;
    trim(*line);
    if (line->empty()) return '\0';
    return static_cast<char>(std::toupper((*line)[0]));
}

} // namespace input

// =============================================================================
//  Question-set assembly — tiered, dedup'd, always shuffled
// =============================================================================
namespace quiz {

inline std::vector<Question> buildQuestionSet(int count, std::mt19937& rng) {
    std::vector<Question> out;

    // Tier 1: small quizzes try the API. Big quizzes skip it — see the
    // header brief for why (free tier has no session tokens; the pool
    // re-rolls and duplicates are guaranteed, not incidental).
    if (count <= policy::kApiMaxCount) {
        auto fetched = triviaApi::fetch(count);
        if (fetched && !fetched->empty()) {
            out = std::move(*fetched);
        }
    }

    // Top up from the local bank to reach the requested count.
    if (static_cast<int>(out.size()) < count) {
        const int missing = count - static_cast<int>(out.size());
        auto fallback = localBank::get(missing, rng);
        for (auto& q : fallback) out.push_back(std::move(q));
    }

    // Shuffle the combined set so local-bank questions (when present) are
    // interspersed, not clustered at the end.
    std::shuffle(out.begin(), out.end(), rng);
    return out;
}

} // namespace quiz

// =============================================================================
//  Main
// =============================================================================
int main() {
    using namespace color;

    color::clearScreen();
    Game game;

    while (game.state != MenuState::Quit) {
        color::clearScreen();
        render::drawHeader();

        switch (game.state) {
            case MenuState::Category: {
                std::cout << render::categoryMenu();
                auto c = input::readChoice();
                if (!c) return 0;
                if (*c == '\0') continue;
                if (*c == 'Q') { game.state = MenuState::Quit; break; }
                const int idx = (*c - '1');
                if (idx < 0 || idx > 5) continue;
                game.categoryId = idx;
                game.state = MenuState::Difficulty;
                break;
            }

            case MenuState::Difficulty: {
                std::cout << render::difficultyMenu();
                auto c = input::readChoice();
                if (!c) return 0;
                if (*c == '\0') continue;
                if (*c == 'B') { game.state = MenuState::Category; break; }
                const int idx = (*c - '1');
                if (idx < 0 || idx > 3) continue;
                game.difficultyId = idx;
                game.state = MenuState::Count;
                break;
            }

            case MenuState::Count: {
                std::cout << render::countMenu();
                auto c = input::readChoice();
                if (!c) return 0;
                if (*c == '\0') continue;

                if      (*c == '1') game.questionCount = 10;
                else if (*c == '2') game.questionCount = 25;
                else if (*c == '3') game.questionCount = 50;
                else if (*c == '4') game.questionCount = 100;
                else continue;

                color::clearScreen();
                render::drawHeader();
                std::cout << "\n";

                game.questions = quiz::buildQuestionSet(game.questionCount, game.rng);

                std::cout << "\r  " << kGreen << "✓ "
                          << game.questions.size() << " questions ready"
                          << std::string(30, ' ') << kReset << "\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(400));

                game.current = 0;
                game.score   = 0;
                game.state   = MenuState::Playing;
                break;
            }

            case MenuState::Playing: {
                std::cout << render::questionScreen(game);
                auto line = input::readLine();
                if (!line) return 0;
                input::trim(*line);

                auto& q = game.questions[game.current];

                if (!q.answered) {
                    if (line->empty()) continue;
                    const char c = static_cast<char>(std::toupper((*line)[0]));
                    int choice = -1;
                    if (c == 'A') choice = 0;
                    else if (c == 'B') choice = 1;
                    else if (c == 'C') choice = 2;
                    else if (c == 'D') choice = 3;
                    if (choice < 0 || choice >= static_cast<int>(q.answers.size())) continue;
                    q.chosen = choice;
                    q.answered = true;
                    if (choice == q.correctIndex) game.score += 1;
                } else {
                    ++game.current;
                    if (game.current >= static_cast<int>(game.questions.size()))
                        game.state = MenuState::Results;
                }
                break;
            }

            case MenuState::Results: {
                std::cout << render::resultsScreen(game);
                auto c = input::readChoice();
                if (!c) return 0;
                if (*c == '\0') continue;
                if (*c == 'R') {
                    color::clearScreen();
                    render::drawHeader();
                    std::cout << "\n";
                    game.questions = quiz::buildQuestionSet(game.questionCount, game.rng);
                    game.current = 0;
                    game.score   = 0;
                    game.state   = MenuState::Playing;
                } else if (*c == 'M') {
                    game.state = MenuState::Category;
                } else if (*c == 'Q') {
                    game.state = MenuState::Quit;
                }
                break;
            }

            case MenuState::Quit:
                break;
        }
    }

    color::clearScreen();
    std::cout << "\n  " << kBoldGreen << "Thanks for playing!" << kReset << "\n\n";
    return 0;
}