#include <iostream>
#include <thread>
#include <string>
#include <mqueue.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <unistd.h>
void listen_queue(const std::string &qname)
{
    mqd_t client_q = mq_open(qname.c_str(), O_RDONLY);
    if (client_q == -1)
    {
        perror("mq_open client listen");
        return;
    }
    char buf[1024];
    while (true)
    {
        ssize_t n = mq_receive(client_q, buf, sizeof(buf), nullptr);
        if (n > 0)
        {
            buf[n] = '\0';
            std::cout << "\n"
                      << buf << "\n> " << std::flush;
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

    std::string client_name = argv[1];
    std::string client_qname = "/client_" + client_name;

    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = 1024;
    attr.mq_curmsgs = 0;

    mqd_t client_q = mq_open(client_qname.c_str(), O_CREAT | O_RDONLY, 0644, &attr);
    if (client_q == -1)
    {
        perror("mq_open client");
        return 1;
    }
    mq_close(client_q);

    std::thread t(listen_queue, client_qname);
    t.detach();

    mqd_t server_q = mq_open("/server", O_WRONLY);
    std::string reg_msg = "REGISTER:" + client_qname;
    mq_send(server_q, reg_msg.c_str(), reg_msg.size() + 1, 0);

    std::cout << "Registered as " << client_name << "\n> ";
    std::string msg;
    while (std::getline(std::cin, msg))
    {
        if (msg.rfind("SAY:", 0) == 0)
        {
            std::string send_msg = "SAY:[" + client_name + "]: " + msg.substr(4);
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }
        if (msg.rfind("JOIN:", 0) == 0)
        {
            std::string send_msg = "JOIN:" + client_name + ": " + msg.substr(5);
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }
        std::cout << "> " << std::flush;
    }
    mq_close(server_q);
    mq_unlink(client_qname.c_str());
    return 0;
}