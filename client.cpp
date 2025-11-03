#include <iostream>
#include <thread>
#include <mqueue.h>
#include <atomic>
#include <chrono>
#include <time.h>
#include <string>

// ============================================================
//  GLOBAL VARIABLES AND CONSTANTS
// ============================================================

std::atomic<bool> keep_running(true);

#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_RESET "\x1b[0m"

// ============================================================
//  HEARTBEAT SYSTEM
// ============================================================

void heartbeat_sender(const std::string &client_name, const std::string &server_qname)
{
    const std::string ping_msg = "PING:" + client_name;

    while (keep_running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(10)); // ส่ง ping ทุก 10 วิ
        if (!keep_running)
            break;

        // เปิดคิวของ server เพื่อส่ง ping
        mqd_t server_q = mq_open(server_qname.c_str(), O_WRONLY | O_NONBLOCK);
        mq_send(server_q, ping_msg.c_str(), ping_msg.size() + 1, 0);
        mq_close(server_q);
    }
}

// ============================================================
//  LISTENER THREAD
// ============================================================

void listen_queue(const std::string &qname)
{
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = 1024;
    attr.mq_curmsgs = 0;

    // เปิดคิวของ client เพื่อรอรับข้อความ
    mqd_t client_q = mq_open(qname.c_str(), O_CREAT | O_RDONLY, 0644, &attr);

    char buf[1024];
    while (keep_running)
    {
        // รอสูงสุด 2 วิ แล้ววนใหม่ (เพื่อให้ thread ปิดได้เร็วเมื่อ quit)
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;

        ssize_t n = mq_timedreceive(client_q, buf, sizeof(buf), nullptr, &ts);
        if (n > 0)
        {
            buf[n] = '\0';
            std::cout << "\n"
                      << ANSI_COLOR_YELLOW << buf << ANSI_COLOR_RESET << "\n"
                      << ANSI_COLOR_GREEN << "> " << ANSI_COLOR_RESET << std::flush;
        }
    }
    mq_close(client_q);
}

// ============================================================
//  CONFIRMATION  FOR QUIR OR LEAVE
// ============================================================

bool confirm_action(const std::string &action)
{
    std::cout << ANSI_COLOR_YELLOW
              << "Are you sure you want to " << action << "? (y/n): "
              << ANSI_COLOR_RESET;

    std::string confirm_line;
    std::getline(std::cin, confirm_line);

    if (confirm_line == "y" || confirm_line == "Y")
        return true;

    std::cout << "Action cancelled." << std::endl;
    return false;
}

// ============================================================
//  COMMAND FORMATION
// ============================================================

void innitial_commands()
{
    std::cout << "+++++     commands for chat server    +++++" << std::endl;
    std::cout << "===========================================" << std::endl;
    std::cout << "===== JOIN  -- JOIN:<room_name>       =====" << std::endl;
    std::cout << "===== SAY   -- SAY:<message>          =====" << std::endl;
    std::cout << "===== DM    -- DM:<target>:<message>  =====" << std::endl;
    std::cout << "===== WHO   -- WHO:                   =====" << std::endl;
    std::cout << "===== LEAVE -- LEAVE:                 =====" << std::endl;
    std::cout << "===== QUIT  -- QUIT:                  =====" << std::endl;
    std::cout << "===========================================" << std::endl;
}

// ============================================================
//  MAIN FUNCTION
// ============================================================

int main(int argc, char *argv[])
{
    // ตรวจสอบการใช้งาน
    if (argc < 2)
    {
        std::cerr << "+++++ USAGE: ./<client_file> <client_name> +++++" << std::endl;
        return 1;
    }

    // เก็บข้อมูล client
    std::string client_name = argv[1];
    std::string client_qname = "/client_" + client_name;
    std::string current_room = "";

    // เริ่ม thread สำหรับรับข้อความจาก server
    std::thread listener_thread(listen_queue, client_qname);

    // สร้าง queue สำหรับส่งไป server
    mqd_t server_q = mq_open("/server", O_WRONLY);
    std::string reg_msg = "REGISTER:" + client_qname;
    mq_send(server_q, reg_msg.c_str(), reg_msg.size() + 1, 0);

    // เริ่ม thread heartbeat
    std::thread heartbeat_thread(heartbeat_sender, client_name, "/server");

    // startting client interface
    system("clear");
    std::cout << std::endl;
    innitial_commands();
    std::cout << std::endl;
    std::cout << "REGISTERED AS " << client_name << std::endl;
    std::cout << std::endl;
    std::cout << ANSI_COLOR_GREEN << "> " << ANSI_COLOR_RESET << std::flush;

    // while loop get input from client keyboard
    std::string msg;
    while (std::getline(std::cin, msg))
    {
        // -----------------------------
        // Command: SAY
        // -----------------------------
        if (msg.rfind("SAY:", 0) == 0)
        {
            std::string send_msg = "SAY:[" + client_name + "]: " + msg.substr(4);
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }
        // -----------------------------
        // Command: JOIN
        // -----------------------------
        else if (msg.rfind("JOIN:", 0) == 0)
        {
            current_room = msg.substr(5);
            std::string send_msg = "JOIN:" + client_name + ": " + current_room;
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
            system("clear");
            std::cout << "Joined #" << current_room << " successfully" << std::endl;
        }
        // -----------------------------
        // Command: DM
        // -----------------------------
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
        // -----------------------------
        // Command: WHO
        // -----------------------------
        else if (msg.rfind("WHO:", 0) == 0)
        {
            std::string send_msg = "WHO:" + client_name + ">" + current_room;
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }
        // -----------------------------
        // Command: LEAVE
        // -----------------------------
        else if (msg.rfind("LEAVE:", 0) == 0)
        {
            if (current_room.empty())
            {
                std::cout << "You are not in any room." << std::endl;
            }
            else if (confirm_action("leave room #" + current_room))
            {
                system("clear");
                std::cout << "You left room #" << current_room << std::endl;
                innitial_commands();
                current_room.clear();
                std::string payload = "LEAVE:" + client_name;
                mq_send(server_q, payload.c_str(), payload.size() + 1, 0);
            }
        }
        // -----------------------------
        // Command: QUIT
        // -----------------------------
        else if (msg.rfind("QUIT:", 0) == 0)
        {
            if (!confirm_action("quit the server"))
            {
                std::cout << "> ";
                continue;
            }

            if (!current_room.empty())
            {
                std::string payload_leave = "LEAVE:" + client_name;
                mq_send(server_q, payload_leave.c_str(), payload_leave.size() + 1, 0);
                std::cout << "You left room before quitting." << std::endl;
                current_room.clear();
            }

            std::string payload_quit = "QUIT:" + client_name;
            mq_send(server_q, payload_quit.c_str(), payload_quit.size() + 1, 0);

            keep_running = false;
            break;
        }
        // -----------------------------
        // Unknown Command
        // -----------------------------
        else if (msg.empty())
        {
        }
        else
        {
            std::cout << "Command not found." << std::endl;
        }

        std::cout << ANSI_COLOR_GREEN << "> " << ANSI_COLOR_RESET << std::flush;
    }

    // ปิดการเชื่อมต่อ
    mq_close(server_q);
    listener_thread.join();
    heartbeat_thread.join();
    mq_unlink(client_qname.c_str());
    return 0;
}
