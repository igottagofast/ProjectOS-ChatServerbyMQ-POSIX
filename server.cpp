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

std::map<std::string, std::vector<std::string>> room_members = {
    {"room1", {}},
    {"room2", {}},
    {"room3", {}}};
std::mutex mtx;
std::vector<std::string> client_queues;

void handle_register(const std::string &msg)
{
    std::string qname = msg.substr(9);
    {
        std::lock_guard<std::mutex> lock(mtx);
        client_queues.push_back(qname);
    }
    std::cout << qname << " has join the server!" << std::endl;
}
void handle_join(const std::string &msg)
{
    std::lock_guard<std::mutex> lock(mtx);

    size_t pos = msg.find(':');

    std::string buf_name = msg.substr(0, pos);
    std::string buf_room = msg.substr(pos + 2);

    if (room_members.count(buf_room))
    {
        room_members[buf_room].push_back(buf_name);
    }
    else
    {
        room_members[buf_room];
        room_members[buf_room].push_back(buf_name);
    }

    for (auto &room : room_members)
    {
        std::cout << room.first << ": ";
        for (auto &member : room.second)
        {
            std::cout << member << " ";
        }
        std::cout << std::endl;
    }
}

void broadcast(const std::string &msg)
{
    std::lock_guard<std::mutex> lock(mtx);
    for (const auto &qname : client_queues)
    {
        mqd_t client_q = mq_open(qname.c_str(), O_WRONLY);
        if (client_q != -1)
        {
            mq_send(client_q, msg.c_str(), msg.size() + 1, 0);
            mq_close(client_q);
        }
    }
}

int main()
{
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

    while (true)
    {
        ssize_t n = mq_receive(server_q, buf, sizeof(buf), nullptr);
        if (n > 0)
        {
            buf[n] = '\0';
            std::string msg(buf);

            if (msg.rfind("REGISTER:", 0) == 0)
            {
                std::thread t(handle_register, msg);
                t.detach();
            }
            else if (msg.rfind("JOIN:", 0) == 0)
            {
                std::thread t(handle_join, msg);
                t.detach();
            }
            else if (msg.rfind("SAY:", 0) == 0)
            {
                std::string payload = msg.substr(4);
                broadcast(payload);
            }
            else if (msg.rfind("DM:", 0) == 0)
            {
            }
            else if (msg.rfind("WHO:", 0) == 0)
            {
            }
            else if (msg.rfind("LEAVE:", 0) == 0)
            {
            }
            else if (msg.rfind("QUIT:", 0) == 0)
            {
            }
            else
            {
            }
        }
    }
    mq_close(server_q);
    mq_unlink("/server");
    return 0;
}