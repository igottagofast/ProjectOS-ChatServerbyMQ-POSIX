#Server

```cpp
#include <condition_variable>
#include <iostream>
#include <thread>
#include <vector>
#include <mqueue.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <unistd.h>
#include <mutex>
#include <map>
#include <algorithm>
#include <pthread.h> 
#include <string>
#include <queue>
#include <cstdio>
#include <errno.h>
#include <chrono>
/**
 * @brief "งาน" ที่ Router จะส่งให้ Broadcaster
 */
struct BroadcastTask {
    std::string message_payload; // "SAY:[sender]: text"
    std::string sender_name;
    std::string target_room;
};

/**
 * @brief Thread-safe queue (Monitor Pattern)
 * นี่คือ "สายพานลำเลียง" ระหว่าง Router และ Broadcaster Pool
 */
template<typename T>
class TaskQueue {
public:
    void push(T item) {
        std::unique_lock<std::mutex> lock(mtx);
        queue.push(item);
        lock.unlock();
        cv.notify_one(); 
    }
    T pop() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]{ return !queue.empty(); });
        T item = queue.front();
        queue.pop();
        return item;
    }
private:
    std::queue<T> queue;
    std::mutex mtx;
    std::condition_variable cv; 
};


/**
 * @brief RAII Wrappers สำหรับ pthread_rwlock_t
 * (เพื่อให้ใช้งานง่ายเหมือน std::lock_guard)
 */
class WriteLock {
public:
    WriteLock(pthread_rwlock_t& lock) : _lock(lock) { pthread_rwlock_wrlock(&_lock); }
    ~WriteLock() { pthread_rwlock_unlock(&_lock); }
private:
    pthread_rwlock_t& _lock;
};

class ReadLock {
public:
    ReadLock(pthread_rwlock_t& lock) : _lock(lock) { pthread_rwlock_rdlock(&_lock); }
    ~ReadLock() { pthread_rwlock_unlock(&_lock); }
private:
    pthread_rwlock_t& _lock;
};

std::map<std::string, std::vector<std::string>> room_members = {
    {"room1", {}},
    {"room2", {}},
    {"room3", {}}};
pthread_rwlock_t registry_lock; 

TaskQueue<BroadcastTask> broadcast_queue; 

std::vector<std::string> client_queues;
std::map<std::string, std::chrono::steady_clock::time_point> client_heartbeats;
std::mutex heartbeat_mutex;
/**
 * @brief (Router) อัปเดตเวลาล่าสุดที่ client ส่ง PING
 */
void handle_ping(const std::string& msg)
{
    std::string client_name = msg.substr(5); // "PING:"
    std::lock_guard<std::mutex> lock(heartbeat_mutex);
    client_heartbeats[client_name] = std::chrono::steady_clock::now();
}

/**
 * @brief (Cleaner Thread) ตรวจสอบ client ที่ "ตาย"
 */
void heartbeat_cleaner()
{
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::seconds(15));

        std::vector<std::string> dead_clients;
        auto now = std::chrono::steady_clock::now();
        const int TIMEOUT_SECONDS = 30; // หมดเวลาถ้าไม่ PING ใน 30 วิ

        {
            std::lock_guard<std::mutex> lock(heartbeat_mutex);
            for (const auto& pair : client_heartbeats) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - pair.second).count();
                if (elapsed > TIMEOUT_SECONDS) {
                    dead_clients.push_back(pair.first);
                }
            }
        } 
        for (const std::string& client_name : dead_clients) {
        std::cout << "[SYSTEM] Heartbeat timeout for " << client_name << ". Cleaning up." << std::endl;
    
        mqd_t server_q = mq_open("/server", O_WRONLY | O_NONBLOCK);
            if (server_q != -1) {
            std::string quit_msg = "QUIT:" + client_name;
            mq_send(server_q, quit_msg.c_str(), quit_msg.size() + 1, 0);
            mq_close(server_q);
            }
        }
    }
}

void handle_register(const std::string &msg)
{
    std::string qname = msg.substr(9);
    {
        WriteLock lock(registry_lock);
        client_queues.push_back(qname);
    }
    std::string client_name = qname.substr(8); {
    std::lock_guard<std::mutex> lock(heartbeat_mutex);
    client_heartbeats[client_name] = std::chrono::steady_clock::now();
    }
    std::cout << qname << " has join the server!" << std::endl;
}

void handle_join(const std::string &msg)
{
    WriteLock lock(registry_lock);

    std::string payload = msg.substr(5);
    size_t pos = payload.find(':');
    if (pos == std::string::npos)
        return;
    std::string name = payload.substr(0, pos);
    std::string room = payload.substr(pos + 2);

    if (!room_members.count(room))
        room_members[room] = {};

    for (auto &pair : room_members)
    {
        auto &members = pair.second;
        members.erase(std::remove(members.begin(), members.end(), name), members.end());
    }

    room_members[room].push_back(name);
    
    BroadcastTask task;
    task.message_payload = "[SYSTEM]: " + name + " has joined #" + room;
    task.sender_name = name;
    task.target_room = room;
    broadcast_queue.push(task);

    std::cout << "\n=== Room Members ===\n";
    for (auto &room_pair : room_members)
    {
        std::cout << room_pair.first << ": ";
        for (auto &member : room_pair.second)
        {
            std::cout << member << " ";
        }
        std::cout << std::endl;
    }
    std::cout << "====================\n";
}

/**
 * @brief นี่คือ "คนงาน" ใน Broadcaster Pool
 * จะรันวน loop ตลอดเวลาเพื่อรอ "งาน" จาก broadcast_queue
 */

void broadcaster_worker()
{
    std::cout << "Broadcaster thread " << std::this_thread::get_id() << " started." << std::endl;

    while (true) 
    {
        BroadcastTask task = broadcast_queue.pop();
        std::string room_to_broadcast;

        // === A+ LOGIC (ส่วนที่ 1) ===
        if (!task.target_room.empty())
        {
            // 1. นี่คือ System Event (JOIN/LEAVE)
            room_to_broadcast = task.target_room;
        }
        else
        {
            // 2. นี่คือ SAY Message (หาห้องเอง)
            {
                ReadLock lock(registry_lock);
                for (const auto &pair : room_members) {
                    const auto &members = pair.second;
                    if (std::find(members.begin(), members.end(), task.sender_name) != members.end()) {
                        room_to_broadcast = pair.first;
                        break;
                    }
                }
            }
        }

        if (room_to_broadcast.empty()) {
            std::cout << "Broadcaster: No room found for task. (Sender: " << task.sender_name << ")" << std::endl;
            continue;
        }

        {
            ReadLock lock(registry_lock);

            // ตรวจสอบเผื่อห้องถูกลบไประหว่างรอ
            if (room_members.find(room_to_broadcast) == room_members.end()) continue;

            for (const auto &member : room_members.at(room_to_broadcast)) {

                // A+ LOGIC (ส่วนที่ 2): ไม่ส่ง System Event ให้ตัวเอง
                if (member == task.sender_name) continue;

                std::string qname = "/client_" + member;
                // ใช้ Error Handling ที่ยอดเยี่ยมของคุณ (O_NONBLOCK)
                mqd_t client_q = mq_open(qname.c_str(), O_WRONLY | O_NONBLOCK); 

                if (client_q != -1) {
                    if (mq_send(client_q, task.message_payload.c_str(), task.message_payload.size() + 1, 0) == -1) {
                        if (errno == EAGAIN) {
                            // (Queue เต็ม) แจ้งเตือน Sender
                            std::string warning = "[Server]: Message to " + member + " dropped (queue full)";
                            std::string sender_q = "/client_" + task.sender_name;
                            mqd_t notify_q = mq_open(sender_q.c_str(), O_WRONLY | O_NONBLOCK);
                            if (notify_q != -1) {
                                mq_send(notify_q, warning.c_str(), warning.size() + 1, 0);
                                mq_close(notify_q);
                            }
                        }
                    }
                    mq_close(client_q);
                } else {
                    std::cerr << "Broadcaster mq_open failed for " << member << std::endl;
                }
            }
        }
    }
}

void handle_dm(const std::string &msg)
{
    ReadLock lock(registry_lock);
    size_t first = msg.find(':', 3);
    if (first == std::string::npos)
        return;

    std::string rest = msg.substr(first + 1);
    size_t second = rest.find(':');
    if (second == std::string::npos)
        return;

    std::string sender = msg.substr(3, first - 3);
    std::string target = rest.substr(0, second);
    std::string message = rest.substr(second + 1);

    std::string qname = "/client_" + target;
    mqd_t client_q = mq_open(qname.c_str(), O_WRONLY);

    if (client_q == -1)
    {
        std::string fail = "[Server]: user '" + target + "' not found.";
        std::string sender_q = "/client_" + sender;
        mqd_t s_q = mq_open(sender_q.c_str(), O_WRONLY);
        if (s_q != -1)
        {
            mq_send(s_q, fail.c_str(), fail.size() + 1, 0);
            mq_close(s_q);
        }
        return;
    }

    std::string full_msg = "[DM from " + sender + "]: " + message;
    mq_send(client_q, full_msg.c_str(), full_msg.size() + 1, 0);
    mq_close(client_q);

    std::cout << sender << " → " << target << " : " << message << std::endl;
}

void handle_who(const std::string &msg)
{
    ReadLock lock(registry_lock);

    size_t name_start = 4;
    size_t end = msg.find('>', name_start);
    if (end == std::string::npos) {
        std::cerr << "Invalid WHO message format\n";
        return;
    }

    std::string client_name = msg.substr(name_start, end - name_start);
    std::string room = msg.substr(end + 1);

    std::string payload = "[Members in #" + room + "]: ";
    
    if (room_members.count(room) && !room_members[room].empty()) {
        for (size_t i = 0; i < room_members[room].size(); ++i) {
            payload += room_members[room][i];
            if (i < room_members[room].size() - 1) payload += ", ";
        }
    } else {
        payload += "(empty)";
    }

    std::string qname = "/client_" + client_name;
    mqd_t client_q = mq_open(qname.c_str(), O_WRONLY);
    if (client_q == -1) {
        perror("mq_open client error");
        return;
    }

    mq_send(client_q, payload.c_str(), payload.size() + 1, 0);
    mq_close(client_q);
}

// ...existing code...
void handle_quit(const std::string &msg)
{
    // msg format: "QUIT:<name>" or "QUIT:<name>:<mode>"
    std::string rest = msg.substr(5); // after "QUIT:"
    std::string client_name;
    std::string mode;

    size_t colon = rest.find(':');
    if (colon == std::string::npos) {
        client_name = rest;
        mode = "quit"; // default: announce quit only
    } else {
        client_name = rest.substr(0, colon);
        mode = rest.substr(colon + 1);
    }

    std::string room_left;

    { 
        WriteLock lock(registry_lock);

        for (auto &pair : room_members)
        {
            auto &members = pair.second;
            auto it = std::remove(members.begin(), members.end(), client_name);

            if (it != members.end()) { 
                members.erase(it, members.end());
                room_left = pair.first; 
                break; 
            }
        }
        std::string qname = "/client_" + client_name;
        client_queues.erase(std::remove(client_queues.begin(), client_queues.end(), qname), client_queues.end());

    }

    std::cout << client_name << " has quit the server." << std::endl;

    // Behavior based on mode:
    // - "quit" (default): announce only quit to the room if user was in one
    // - "leave_then_quit": announce left #room then announce quit to that room
    if (mode == "leave_then_quit") {
        if (!room_left.empty()) {
            BroadcastTask left_task;
            left_task.sender_name = client_name;
            left_task.message_payload = "[SYSTEM]: " + client_name + " has left #" + room_left;
            left_task.target_room = room_left;
            broadcast_queue.push(left_task);
        }
        if (!room_left.empty()) {
            BroadcastTask quit_task;
            quit_task.sender_name = client_name;
            quit_task.message_payload = "[SYSTEM]: " + client_name + " has quit";
            quit_task.target_room = room_left;
            broadcast_queue.push(quit_task);
        }
    } else { // "quit"
        if (!room_left.empty()) {
            BroadcastTask quit_task;
            quit_task.sender_name = client_name;
            quit_task.message_payload = "[SYSTEM]: " + client_name + " has quit";
            quit_task.target_room = room_left;
            broadcast_queue.push(quit_task);
        }
    }

    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex);
        client_heartbeats.erase(client_name);
    }
}


void handle_leave(const std::string &msg)
{
    std::string client_name = msg.substr(6);
    WriteLock lock(registry_lock);

    std::string room_left;
    for (auto &pair : room_members) {
        auto &members = pair.second;
        auto it = std::remove(members.begin(), members.end(), client_name);
        
        if (it != members.end()) {
            members.erase(it, members.end());
            room_left = pair.first;
            std::cout << client_name << " has left " << room_left << std::endl;
            
            BroadcastTask task;
        //task.message_payload = "[SYSTEM]: " + client_name + " has quit"; 
        task.message_payload = "[SYSTEM]: " + client_name + " has left #" + room_left; 
            task.sender_name = client_name;
            task.target_room = room_left;
            broadcast_queue.push(task);
            
            break;
        }
    }
}

int main() {
    pthread_rwlock_init(&registry_lock, NULL);
    const int NUM_BROADCASTERS = 4;
    std::vector<std::thread> workers;
    for (int i = 0; i < NUM_BROADCASTERS; ++i) {
        workers.emplace_back(broadcaster_worker);
        workers.back().detach();
    }
    std::cout << "Broadcaster pool (size=" << NUM_BROADCASTERS << ") started." << std::endl;

    std::thread cleaner_thread(heartbeat_cleaner);
    cleaner_thread.detach();
    std::cout << "Heartbeat cleaner thread started." << std::endl;

    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = 1024;
    attr.mq_curmsgs = 0;

    mqd_t server_q = mq_open("/server", O_CREAT | O_RDWR, 0644, &attr);
    if (server_q == -1)
    {
        perror("mq_open not complete");
        return 1;
    }
    std::cout << "server opened" << std::endl;

    char buf[1024];
    while (true) {
    ssize_t n = mq_receive(server_q, buf, sizeof(buf), nullptr);
    if (n > 0)
    {
        buf[n] = '\0';
        std::string msg(buf);

        if (msg.rfind("REGISTER:", 0) == 0) {
            handle_register(msg);
        }
        else if (msg.rfind("JOIN:", 0) == 0) {
            handle_join(msg);
        }
        else if (msg.rfind("SAY:", 0) == 0) {
            std::string payload = msg.substr(4);
            size_t start = payload.find('[');
            size_t end = payload.find(']');
            std::string sender = payload.substr(start + 1, end - start - 1);
            
            BroadcastTask task;
            task.message_payload = payload;
            task.sender_name = sender;
            task.target_room = "";
            broadcast_queue.push(task);
        }
        else if (msg.rfind("DM:", 0) == 0) {
            handle_dm(msg);
        }
        else if (msg.rfind("WHO:", 0) == 0) {
            handle_who(msg);
        }
        else if (msg.rfind("LEAVE:", 0) == 0) {
            handle_leave(msg);
        }
        else if (msg.rfind("QUIT:", 0) == 0) {
            handle_quit(msg);
        }
        else if (msg.rfind("PING:", 0) == 0) {
            handle_ping(msg);
        }
        else {
            std::cout << "Unknown message: " << msg << std::endl;
        }
    }
} 
    mq_close(server_q);
    mq_unlink("/server");
    return 0;
}
```
---
#Client

```cpp
#include <iostream>
#include <thread>
#include <string>
#include <mqueue.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <unistd.h>
#include <atomic>
#include <chrono>

std::atomic<bool> keep_running(true);

// เพิ่มฟังก์ชันนี้
void heartbeat_sender(std::string client_name, std::string server_qname)
{
    const std::string ping_msg = "PING:" + client_name;

    while (keep_running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (!keep_running) break;

        mqd_t server_q = mq_open(server_qname.c_str(), O_WRONLY | O_NONBLOCK);
        if (server_q == -1) {
            continue; 
        }
        
        if (mq_send(server_q, ping_msg.c_str(), ping_msg.size() + 1, 0) == -1) {
            // ถ้าส่งไม่ได้ ไม่เป็นไร
        }
        mq_close(server_q);
    }
}
// ANSI color codes
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_RESET   "\x1b[0m"

// function to listen to messages from server
void listen_queue(const std::string &qname)
{
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = 1024;
    attr.mq_curmsgs = 0;
    // open client queue for listening
    mqd_t client_q = mq_open(qname.c_str(), O_CREAT | O_RDONLY, 0644, &attr);
    if (client_q == -1)
    {
        perror("mq_open client not complete.");
        return;
    }
    // listen loop
    char buf[1024];
    while (keep_running)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2; // รอสูงสุด 2 วินาที

        ssize_t n = mq_receive(client_q, buf, sizeof(buf), nullptr, &ts);
        if (n > 0)
        {
            buf[n] = '\0';
            std::cout << "\n" << ANSI_COLOR_YELLOW << buf << ANSI_COLOR_RESET 
                  << "\n" << ANSI_COLOR_GREEN << "> " << ANSI_COLOR_RESET << std::flush;
        }
    }
    mq_close(client_q);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "USAGE!!!    --->    ./client [client_name]" << std::endl;
        return 1;
    }
    // keep client name
    std::string client_name = argv[1];
    std::string client_qname = "/client_" + client_name;
    std::string current_room = "";

    // thread listen_queue from server
    std::thread t(listen_queue, client_qname);
    //t.detach();

    //std::thread hb_thread(heartbeat_sender, client_name, "/server");
    //hb_thread.detach();

    // make queue for sending
    mqd_t server_q = mq_open("/server", O_WRONLY);
    std::string reg_msg = "REGISTER:" + client_qname;
    mq_send(server_q, reg_msg.c_str(), reg_msg.size() + 1, 0);
    std::cout << "Registered as " << client_name << "\n" 
              << ANSI_COLOR_GREEN << "> " << ANSI_COLOR_RESET << std::flush;

    std::thread pinger(heartbeat_sender, client_name, "/server");

    auto confirm_action = [](const std::string& action) -> bool {
        std::cout << ANSI_COLOR_YELLOW << "Are you sure you want to " << action << "? (y/n): " << ANSI_COLOR_RESET;
        std::string confirm_line;
        std::getline(std::cin, confirm_line);
        if (confirm_line == "y" || confirm_line == "Y") {
            return true;
        }
        std::cout << "Action cancelled." << std::endl;
        return false;
    };

    std::string msg;
    while (std::getline(std::cin, msg))
    {
        if (msg.rfind("SAY:", 0) == 0)
        {
            std::string send_msg = "SAY:[" + client_name + "]: " + msg.substr(4);
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }
        else if (msg.rfind("JOIN:", 0) == 0)
        {
            current_room = msg.substr(5);
            std::string send_msg = "JOIN:" + client_name + ": " + current_room;
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);

            system("clear");
            std::cout << "Joined   # " + current_room + "   successfully" << std::endl;
        }
        else if (msg.rfind("DM:", 0) == 0)
        {
            size_t pos = msg.find(':', 3);
            if (pos == std::string::npos)
            {
                std::cout << "Invalid DM format. Use: DM:<target>:<message>\n> ";
                continue;
            }

            std::string target = msg.substr(3, pos - 3);
            std::string text = msg.substr(pos + 1);

            std::string send_msg = "DM:" + client_name + ":" + target + ":" + text;
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }
       
        else if (msg.rfind("WHO:", 0) == 0)
        {
            std::string send_msg = "WHO:" + client_name + ">" + current_room;
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }
    
        else if (msg.rfind("LEAVE:", 0) == 0)
        {
            if (current_room.empty()) {
                std::cout << "You are not in any room." << std::endl;
            } else if (confirm_action("leave room #" + current_room)) {
                std::string room_that_left = current_room;
                current_room = "";
                std::string payload = "LEAVE:" + client_name;
                mq_send(server_q, payload.c_str(), payload.size() + 1, 0);
                std::cout << "You left room #" << room_that_left << std::endl;
            }
        }
        else if (msg.rfind("QUIT:", 0) == 0) {
            if (confirm_action("quit the server")) {
                
                std::string payload;

                // ถ้าอยู่ในห้อง, ให้ถามวิธีออก
                if (!current_room.empty()) {
                    std::cout << ANSI_COLOR_YELLOW << "How do you want to quit?" << ANSI_COLOR_RESET << std::endl;
                    std::cout << "1. Just quit (Announce 'quit' to room)" << std::endl;
                    std::cout << "2. Announce 'leave' then 'quit' (Two messages)" << std::endl;
                    std::cout << ANSI_COLOR_GREEN << "Enter choice (1 or 2): " << ANSI_COLOR_RESET << std::flush;
                    
                    std::string choice_line;
                    std::getline(std::cin, choice_line);
                    
                    if (choice_line == "2") {
                        // นี่คือทางเลือกที่ 2
                        payload = "QUIT:" + client_name + ":leave_then_quit";
                    } else {
                        // ถ้าพิมพ์ 1 หรืออื่นๆ, ใช้ค่า default
                        // นี่คือทางเลือกที่ 1
                        payload = "QUIT:" + client_name;
                    }
                } else {
                    // ถ้าไม่อยู่ในห้อง, ก็ส่ง quit ธรรมดา
                    // นี่คือทางเลือกที่ 1
                    payload = "QUIT:" + client_name;
                }

                keep_running = false; // บอก thread อื่นๆ ให้หยุด
                current_room = ""; 
            
                mq_send(server_q, payload.c_str(), payload.size() + 1, 0);
                break; // ออกจาก loop (main thread)
            }
        }
        else {
            std::cout << "command not found" << std::endl;
        }
        std::cout << ANSI_COLOR_GREEN << "> " << ANSI_COLOR_RESET << std::flush;
    }
    mq_close(server_q);
    t.join();
    pinger.join();
    mq_unlink(client_qname.c_str());
    return 0;
}
```
---
#How to complie



