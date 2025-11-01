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
