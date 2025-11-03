Server
---

ระบบ Server ในโปรแกรมนี้ทำหน้าที่เป็นศูนย์กลางของการสื่อสารระหว่างผู้ใช้ (client) หลายคน โดยผู้ใช้แต่ละรายจะส่งคำสั่งมายังเซิร์ฟเวอร์ผ่าน POSIX Message Queue (MQ) เพื่อให้เซิร์ฟเวอร์ประมวลผลและตอบสนองตามคำสั่งที่ได้รับ เช่น การเข้าระบบ (REGISTER), การเข้าห้อง (JOIN), การส่งข้อความ (SAY), การส่งข้อความส่วนตัว (DM), การขอดูรายชื่อสมาชิก (WHO), การออกจากห้อง (LEAVE), การออกจากระบบ (QUIT) รวมถึงการส่งสัญญาณตรวจสอบสถานะ (PING) เพื่อบอกว่า client ยังออนไลน์อยู่

เซิร์ฟเวอร์โปรเเกรมนี้ออกแบบให้มี การทำงานแบบขนาน (Concurrent Processing) โดยแบ่งโครงสร้างออกเป็น 3 ส่วนหลัก ซึ่งภายในแต่ละส่วนยังแยกย่อยออกเป็น 7 พาร์ทย่อย ที่รับผิดชอบหน้าที่เฉพาะของตนเอง ดังนี้

ส่วนที่ 1: ระบบโครงสร้างพื้นฐานและสถานะกลางของ Server (Infrastructure & Global State)

ส่วนนี้รวมพาร์ทย่อยที่เกี่ยวข้องกับการประกาศโครงสร้างข้อมูล การสร้างคลาสเครื่องมือ และการเก็บสถานะของระบบทั้งหมด

---
พาร์ทที่ 1 ส่วนประกาศ header, struct, และ utility : เซิร์ฟเวอร์ “ไม่ได้ส่งให้ client ตรง ๆ ทันที” แต่จะสร้าง BroadcastTask แล้วโยนเข้า broadcast_queue เพื่อให้ worker thread ไปส่งทีหลัง → ทำให้ main thread ไม่ค้าง
ใช้ RW-lock เพราะรายการสมาชิกห้องถูก “อ่าน” บ่อยกว่าถูก “เขียน”
```cpp

// งาน 1 ชิ้นที่ต้องกระจายออกไปให้สมาชิกในห้อง
struct BroadcastTask
{
    std::string message_payload; // ข้อความที่จะส่งจริง เช่น "[SYSTEM]: A joined #room1"
    std::string sender_name;     // ใครเป็นคนส่ง (ใช้กันส่งกลับให้ตัวเอง)
    std::string target_room;     // ถ้าระบุ แปลว่าต้องส่งให้ห้องนี้เลย
};

// คิวงานแบบปลอดภัยสำหรับหลายเธรด (Producer-Consumer)
template <typename T>
class TaskQueue
{
public:
    void push(T item)
    {
        std::unique_lock<std::mutex> lock(mtx);
        queue.push(item);
        lock.unlock();
        cv.notify_one();              // ปลุก worker ที่กำลังรอ pop()
    }

    T pop()
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]{ return !queue.empty(); }); // รอจนกว่าจะมีงาน
        T item = queue.front();
        queue.pop();
        return item;
    }

private:
    std::queue<T> queue;
    std::mutex mtx;
    std::condition_variable cv;
};

// ตัวช่วยจับ rwlock แบบ RAII เพื่อให้เขียนโค้ดสั้นและปลอดภัย
class WriteLock
{
public:
    WriteLock(pthread_rwlock_t &lock) : _lock(lock) { pthread_rwlock_wrlock(&_lock); }
    ~WriteLock() { pthread_rwlock_unlock(&_lock); }
private:
    pthread_rwlock_t &_lock;
};

class ReadLock
{
public:
    ReadLock(pthread_rwlock_t &lock) : _lock(lock) { pthread_rwlock_rdlock(&_lock); }
    ~ReadLock() { pthread_rwlock_unlock(&_lock); }
private:
    pthread_rwlock_t &_lock;
};


```
พาร์ทที่ 2 Global State : เก็บสถานะเซิร์ฟเวอร์ทั้งหมด ที่ทุกเธรดต้องใช้ร่วมกัน 

room_members คือแหล่งจริงที่บอกว่า “ตอนนี้ใครอยู่ห้องไหน” → broadcaster ต้องอ่านจากตรงนี้

heartbeat แยก mutex ของตัวเองออกมาเลย เพราะมันเป็นงานอีกชุดที่ไม่เกี่ยวกับ RW-lock ของห้อง
```cpp

// ตารางห้อง → รายชื่อสมาชิกในห้อง
std::map<std::string, std::vector<std::string>> room_members = {
    {"room1", {}}, {"room2", {}}, {"room3", {}}};

// lock สำหรับป้องกันการแก้ไข room_members / client_queues พร้อมกันหลายเธรด
pthread_rwlock_t registry_lock;

// คิวกลางสำหรับงาน broadcast
TaskQueue<BroadcastTask> broadcast_queue;

// รายการชื่อคิวของ client ที่เคย REGISTER เข้ามา
std::vector<std::string> client_queues;

// เก็บเวลา heartbeat ล่าสุดของ client แต่ละคน
std::map<std::string, std::chrono::steady_clock::time_point> client_heartbeats;
std::mutex heartbeat_mutex;  // ป้องกัน map นี้โดยเฉพาะเพราะใช้บ่อย

```
---
ส่วนที่ 2: ระบบจัดการผู้ใช้และการเชื่อมต่อ (Client Management & Event Handling)

---
พาร์ทที่ 3: ระบบ Heartbeat และการล้าง client : ให้เซิร์ฟเวอร์ “รู้ว่าใครยังมีชีวิตอยู่” และ “เอาคนที่หายไปนานออกให้”

heartbeat_cleaner() — ฝั่งล้างศพ รันเป็น thread แยกตลอดเวลา 

ทุก 15 วินาที เซิร์ฟเวอร์จะเช็ก client ทุกคน ถ้าไม่ส่ง heartbeat ภายใน 30 วินาที จะถือว่า “ตายแล้ว”

ระบบจะเรียก handle_quit() แทนลูกค้า เพื่อเคลียร์ห้อง, broadcast ว่าคนนั้นออก, และลบ heartbeat เซิร์ฟเวอร์สามารถจัดการ client ที่ disconnect ไปเงียบ ๆ โดยอัตโนมัติ → ไม่มีผีค้างห้อง

```cpp
void handle_quit(const std::string &msg);  // ต้องประกาศก่อน เพราะ heartbeat_cleaner เรียกใช้

// อัปเดตว่า client คนนี้เพิ่งส่ง PING มา
void handle_ping(const std::string &msg)
{
    std::string client_name = msg.substr(5); // ตัด "PING:"
    std::lock_guard<std::mutex> lock(heartbeat_mutex);
    client_heartbeats[client_name] = std::chrono::steady_clock::now();
}

// เธรดนี้จะคอยตรวจทุก 15 วิ ว่ามี client ไหนไม่ส่ง PING เกิน 30 วิไหม
void heartbeat_cleaner()
{
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::seconds(15));

        std::vector<std::string> dead_clients;
        auto now = std::chrono::steady_clock::now();
        const int TIMEOUT_SECONDS = 30;

        // หา client ที่ตาย
        {
            std::lock_guard<std::mutex> lock(heartbeat_mutex);
            for (const auto &pair : client_heartbeats)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - pair.second).count();
                if (elapsed > TIMEOUT_SECONDS)
                    dead_clients.push_back(pair.first);
            }
        }

        // เคลียร์ client ที่ตายแล้ว ออกจากระบบเหมือน QUIT
        for (const std::string &client_name : dead_clients)
        {
            std::cout << "[SYSTEM] Heartbeat timeout for " << client_name << ". Cleaning up." << std::endl;
            handle_quit("QUIT:" + client_name);

            // แจ้ง main ผ่านคิว /server ด้วย เผื่อ logic อื่นต้องรู้
            mqd_t server_q = mq_open("/server", O_WRONLY | O_NONBLOCK);
            if (server_q != -1)
            {
                std::string quit_msg = "QUIT:" + client_name;
                mq_send(server_q, quit_msg.c_str(), quit_msg.size() + 1, 0);
                mq_close(server_q);
            }
        }
    }
}

```

พาร์ทที่ 4 กลุ่ม Handler สำหรับคำสั่งจาก client : แปลง “ข้อความที่ client ส่งมา” ให้กลายเป็นการอัปเดตสถานะ + ถ้าต้องกระจายก็โยนงานเข้า broadcast_queue

 ได้เเก่ register, join, say, dm, who, leave, quit

 ทุก handler “ไม่ส่งตรง” แต่สร้างงานแล้วโยนให้ Broadcast Worker → main thread เบา

 ```cpp
// client ใหม่ประกาศตัวเข้าระบบ
void handle_register(const std::string &msg)
{
    std::string qname = msg.substr(9);   // "REGISTER:/client_x"
    {
        WriteLock lock(registry_lock);
        client_queues.push_back(qname);
    }

    // เก็บเวลา heartbeat แรกให้เลย
    std::string client_name = qname.substr(8); // ตัด "/client_"
    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex);
        client_heartbeats[client_name] = std::chrono::steady_clock::now();
    }
    std::cout << qname << " has joined the server!" << std::endl;
}

// client ขอเข้าห้อง / ย้ายห้อง
void handle_join(const std::string &msg)
{
    WriteLock lock(registry_lock);

    // format: "JOIN:alice: room1"
    std::string payload = msg.substr(5);
    size_t pos = payload.find(':');
    if (pos == std::string::npos)
        return;

    std::string name = payload.substr(0, pos);
    std::string room = payload.substr(pos + 2); // ข้าม ": "

    // ถ้าห้องยังไม่มีให้สร้าง
    if (!room_members.count(room))
        room_members[room] = {};

    // เอาออกจากทุกห้องก่อน (กันอยู่หลายห้อง)
    for (auto &pair : room_members)
    {
        auto &members = pair.second;
        members.erase(std::remove(members.begin(), members.end(), name), members.end());
    }

    // ใส่เข้าห้องเป้าหมาย
    room_members[room].push_back(name);

    // สร้างงาน broadcast แจ้งว่ามีคนเข้าห้อง
    BroadcastTask task;
    task.message_payload = "[SYSTEM]: " + name + " has joined #" + room;
    task.sender_name = name;
    task.target_room = room;
    broadcast_queue.push(task);
}

// ส่งข้อความส่วนตัว
void handle_dm(const std::string &msg)
{
    ReadLock lock(registry_lock);

    // format: "DM:alice:bob:hello"
    size_t first = msg.find(':', 3);
    if (first == std::string::npos) return;

    std::string rest = msg.substr(first + 1);
    size_t second = rest.find(':');
    if (second == std::string::npos) return;

    std::string sender  = msg.substr(3, first - 3);
    std::string target  = rest.substr(0, second);
    std::string message = rest.substr(second + 1);

    std::string qname = "/client_" + target;
    mqd_t client_q = mq_open(qname.c_str(), O_WRONLY);

    if (client_q == -1)
    {
        // ส่งกลับไปบอก sender ว่าไม่พบปลายทาง
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

// ขอรายชื่อสมาชิกในห้อง
void handle_who(const std::string &msg)
{
    ReadLock lock(registry_lock);

    // format: "WHO:alice>room1"
    size_t name_start = 4;
    size_t end = msg.find('>', name_start);
    if (end == std::string::npos) return;

    std::string client_name = msg.substr(name_start, end - name_start);
    std::string room        = msg.substr(end + 1);

    std::string payload = "[Members in #" + room + "]: ";

    if (room_members.count(room) && !room_members[room].empty())
    {
        for (size_t i = 0; i < room_members[room].size(); ++i)
        {
            payload += room_members[room][i];
            if (i < room_members[room].size() - 1)
                payload += ", ";
        }
    }
    else
    {
        payload += "(empty)";
    }

    std::string qname = "/client_" + client_name;
    mqd_t client_q = mq_open(qname.c_str(), O_WRONLY);
    if (client_q == -1) return;

    mq_send(client_q, payload.c_str(), payload.size() + 1, 0);
    mq_close(client_q);
}

// ข้อความปกติในห้อง
void handle_say(const std::string &msg)
{
    // msg: "SAY:[alice]: hello"
    std::string payload = msg.substr(4);
    size_t start = payload.find('[');
    size_t end   = payload.find(']');
    std::string sender = payload.substr(start + 1, end - start - 1);

    BroadcastTask task;
    task.message_payload = payload;
    task.sender_name = sender;
    task.target_room = "";  // ให้ worker หาห้องให้เองจาก sender
    broadcast_queue.push(task);
}

// ออกจากห้อง
void handle_leave(const std::string &msg)
{
    std::string client_name = msg.substr(6);
    WriteLock lock(registry_lock);

    for (auto &pair : room_members)
    {
        auto &members = pair.second;
        auto it = std::remove(members.begin(), members.end(), client_name);
        if (it != members.end())
        {
            members.erase(it, members.end());

            BroadcastTask task;
            task.message_payload = "[SYSTEM]: " + client_name + " has left #" + pair.first;
            task.sender_name = client_name;
            task.target_room = pair.first;
            broadcast_queue.push(task);
            break;
        }
    }
}

// ออกจากระบบทั้งหมด
void handle_quit(const std::string &msg)
{
    // msg อาจเป็น "QUIT:alice" หรือ "QUIT:alice:leave_then_quit"
    std::string rest = msg.substr(5);
    std::string client_name;
    std::string mode;

    size_t colon = rest.find(':');
    if (colon == std::string::npos)
    {
        client_name = rest;
        mode = "quit";
    }
    else
    {
        client_name = rest.substr(0, colon);
        mode = rest.substr(colon + 1);
    }

    std::string room_left;

    {
        WriteLock lock(registry_lock);
        // เอา client ออกจากห้องก่อน
        for (auto &pair : room_members)
        {
            auto &members = pair.second;
            auto it = std::remove(members.begin(), members.end(), client_name);

            if (it != members.end())
            {
                members.erase(it, members.end());
                room_left = pair.first;
                break;
            }
        }
        // เอาออกจากลิสต์คิวด้วย
        std::string qname = "/client_" + client_name;
        client_queues.erase(std::remove(client_queues.begin(), client_queues.end(), qname), client_queues.end());
    }

    std::cout << client_name << " has quit the server." << std::endl;

    // แจ้งห้องที่เหลือว่าคนนี้ออกแล้ว
    if (!room_left.empty())
    {
        BroadcastTask quit_task;
        quit_task.sender_name = client_name;
        quit_task.message_payload = "[SYSTEM]: " + client_name + " has quit";
        quit_task.target_room = room_left;
        broadcast_queue.push(quit_task);
    }

    // ลบ heartbeat ออก
    std::lock_guard<std::mutex> lock(heartbeat_mutex);
    client_heartbeats.erase(client_name);
}

 ```

พาร์ท 5 การจัดการห้องและสมาชิก (JOIN / LEAVE / WHO)
- JOIN (ย้ายเข้าห้อง)
```cpp
/* JOIN
   - parse "JOIN:<name>: <room>"
   - เอา <name> ออกจากทุกห้อง
   - ถ้า <room> ยังไม่มีให้สร้าง แล้วใส่ชื่อเข้าไป
   - ส่ง SYSTEM msg ว่าเข้าห้อง (#room)
*/
void handle_join(const std::string &msg)
{
    WriteLock lock(registry_lock);

    // msg รูปแบบ: "JOIN:<name>: <room>"
    std::string payload = msg.substr(5);
    size_t pos = payload.find(':');
    if (pos == std::string::npos) return;

    std::string name = payload.substr(0, pos);
    std::string room = payload.substr(pos + 2);

    if (!room_members.count(room))
        room_members[room] = {};

    // ลบคนนี้ออกจากห้องเก่าก่อน
    for (auto &pair : room_members) {
        auto &members = pair.second;
        members.erase(std::remove(members.begin(), members.end(), name), members.end());
    }

    // ใส่คนนี้เข้าห้องใหม่
    room_members[room].push_back(name);

    // แจ้งทุกคนในห้องว่ามีคนเข้ามา
    BroadcastTask task;
    task.message_payload = "[SYSTEM]: " + name + " has joined #" + room;
    task.sender_name = name;
    task.target_room = room;
    broadcast_queue.push(task);

    // debug แสดงสมาชิกทุกห้อง
    std::cout << "\n=== Room Members ===\n";
    for (auto &room_pair : room_members) {
        std::cout << room_pair.first << ": ";
        for (auto &member : room_pair.second) {
            std::cout << member << " ";
        }
        std::cout << std::endl;
    }
    std::cout << "====================\n";
}


```
- LEAVE (ออกจากห้องปัจจุบัน)
```cpp
// ผู้ใช้ออกจากห้อง และประกาศออกห้อง
void handle_leave(const std::string &msg)
{
    // msg รูปแบบ: "LEAVE:<client_name>"
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
            task.message_payload = "[SYSTEM]: " + client_name + " has left #" + room_left;
            task.sender_name = client_name;
            task.target_room = room_left;
            broadcast_queue.push(task);
            break;
        }
    }
}

```

- WHO (ขอดูว่าใครอยู่ในห้อง)
```cpp
// ตอบรายชื่อสมาชิกในห้องให้ผู้ร้องขอ
void handle_who(const std::string &msg)
{
    ReadLock lock(registry_lock);

    // msg รูปแบบ: "WHO:<client>><room>"
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

```

การจัดการการเชื่อมต่อของผู้ใช้ (REGISTER / QUIT / DM)
- REGISTER (ผู้ใช้เข้าระบบครั้งแรก)
```cpp
// ลงทะเบียนคิวของ client และบันทึก heartbeat แรก
void handle_register(const std::string &msg)
{
    // msg รูปแบบ: "REGISTER:/client_name"
    std::string qname = msg.substr(9);

    {
        WriteLock lock(registry_lock);
        client_queues.push_back(qname);
    }

    // ดึงชื่อ client จาก "/client_xxx"
    std::string client_name = qname.substr(8);

    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex);
        client_heartbeats[client_name] = std::chrono::steady_clock::now();
    }

    std::cout << qname << " has join the server!" << std::endl;
}


```

- DM (ส่งข้อความส่วนตัว)
```cpp
// ส่งข้อความส่วนตัวถึงเป้าหมาย (ถ้าไม่พบให้แจ้งกลับผู้ส่ง)
void handle_dm(const std::string &msg)
{
    ReadLock lock(registry_lock);

    // รูปแบบ: "DM:<sender>:<target>:<message>"
    size_t first = msg.find(':', 3);
    if (first == std::string::npos) return;

    std::string rest = msg.substr(first + 1);
    size_t second = rest.find(':');
    if (second == std::string::npos) return;

    std::string sender = msg.substr(3, first - 3);
    std::string target = rest.substr(0, second);
    std::string message = rest.substr(second + 1);

    std::string qname = "/client_" + target;
    mqd_t client_q = mq_open(qname.c_str(), O_WRONLY);

    if (client_q == -1)
    {
        // ถ้าหา target ไม่เจอ ให้แจ้งกลับ sender
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

```

- QUIT (ผู้ใช้ออกจากระบบ)
```cpp
// ลบผู้ใช้ออกจากทุกที่ และประกาศ quit (เลือกโหมดได้)
void handle_quit(const std::string &msg)
{
    // "QUIT:<name>" หรือ "QUIT:<name>:<mode>"
    std::string rest = msg.substr(5);
    std::string client_name;
    std::string mode;

    size_t colon = rest.find(':');
    if (colon == std::string::npos) {
        client_name = rest;
        mode = "quit"; // default
    } else {
        client_name = rest.substr(0, colon);
        mode = rest.substr(colon + 1);
    }

    std::string room_left;

    {
        WriteLock lock(registry_lock);

        // ลบ client ออกจากห้องที่อยู่
        for (auto &pair : room_members) {
            auto &members = pair.second;
            auto it = std::remove(members.begin(), members.end(), client_name);

            if (it != members.end()) {
                members.erase(it, members.end());
                room_left = pair.first;
                break;
            }
        }

        // ลบชื่อคิวของ client ออกจากรายการ client_queues
        std::string qname = "/client_" + client_name;
        client_queues.erase(
            std::remove(client_queues.begin(), client_queues.end(), qname),
            client_queues.end()
        );
    }

    std::cout << client_name << " has quit the server." << std::endl;

    // Broadcast ออกจากห้อง / ออกจากระบบ แบบเลือกโหมด
    if (mode == "leave_then_quit")
    {
        if (!room_left.empty())
        {
            BroadcastTask left_task;
            left_task.sender_name = client_name;
            left_task.message_payload = "[SYSTEM]: " + client_name + " has left #" + room_left;
            left_task.target_room = room_left;
            broadcast_queue.push(left_task);
        }
        if (!room_left.empty())
        {
            BroadcastTask quit_task;
            quit_task.sender_name = client_name;
            quit_task.message_payload = "[SYSTEM]: " + client_name + " has quit";
            quit_task.target_room = room_left;
            broadcast_queue.push(quit_task);
        }
    }
    else // mode == "quit"
    {
        if (!room_left.empty())
        {
            BroadcastTask quit_task;
            quit_task.sender_name = client_name;
            quit_task.message_payload = "[SYSTEM]: " + client_name + " has quit";
            quit_task.target_room = room_left;
            broadcast_queue.push(quit_task);
        }
    }

    // เอา client นี้ออกจากตาราง heartbeat
    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex);
        client_heartbeats.erase(client_name);
    }
}

```
---
ส่วนที่ 3: ระบบประมวลผลและกระจายข้อความ (Processing & Broadcast System)
---
พาร์ท 6 Broadcast System : Broadcast System คือส่วนที่รับผิดชอบการ “กระจายข้อความ” ที่เกิดขึ้นในระบบแชท ไปยังผู้ใช้คนอื่น ๆ ที่อยู่ในห้องเดียวกัน โดยออกแบบให้แยกออกจากส่วนหลักของเซิร์ฟเวอร์ (ส่วนที่รอรับคำสั่งจาก client) เพื่อไม่ให้การส่งข้อความไปยังหลาย ๆ คนทำให้เซิร์ฟเวอร์หลักค้าง การออกแบบนี้ใช้แนวคิด Producer–Consumer: ส่วน main/handler จะเป็น Producer สร้าง “งานกระจายข้อความ” แล้วส่งเข้า คิวกลาง ส่วนกลุ่มเธรด Broadcaster จะเป็น Consumer คอยดึงงานออกมาส่งจริง

   6.1 โครงสร้างงานกระจายข้อความ
   
   main/handler = producer → เรียก broadcast_queue.push(task);

   broadcaster worker = consumer → เรียก broadcast_queue.pop();

   ใช้ condition_variable → worker ไม่ต้องวนลูปเช็คตลอด (ไม่ busy-wait)
 ```cpp
struct BroadcastTask
{
    std::string message_payload; // เนื้อความที่จะส่ง เช่น "[alice]: hi" หรือ "[SYSTEM]: bob has left #room1"
    std::string sender_name;     // ชื่อผู้ส่ง ใช้กันไม่ให้ส่งย้อนกลับไปหาคนส่ง
    std::string target_room;     // ห้องเป้าหมาย ถ้าว่างแปลว่าต้องให้ worker เดาห้องจาก sender
};
```
  6.2 คิวงานกลางแบบ Thread-safe

```cpp
// TaskQueue<T> = คิวงานข้ามเธรดแบบปลอดภัย
// ใช้ร่วมกับ condition_variable เพื่อให้ worker รอแบบไม่เปลือง CPU
template <typename T>
class TaskQueue
{
public:
    void push(T item)
    {
        std::unique_lock<std::mutex> lock(mtx);
        queue.push(item);        // ใส่งานใหม่
        lock.unlock();           // ปลดล็อกก่อนปลุก worker
        cv.notify_one();         // ปลุก worker ให้มาหยิบงาน
    }

    T pop()
    {
        std::unique_lock<std::mutex> lock(mtx);
        // ถ้ายังไม่มีงาน ให้รอจนกว่าจะมีคน push เข้ามา
        cv.wait(lock, [this]{ return !queue.empty(); });
        T item = queue.front();
        queue.pop();
        return item;
    }

private:
    std::queue<T> queue;             // เก็บงานตามลำดับ FIFO
    std::mutex mtx;                  // กันหลายเธรดแก้คิวพร้อมกัน
    std::condition_variable cv;      // ให้ worker รอแบบบล็อกจนมีงาน
};

// คิวกลางจริงที่เซิร์ฟเวอร์ใช้
TaskQueue<BroadcastTask> broadcast_queue;
```
   6.3 Worker กระจายข้อความ: worker ตัวนี้คือ ‘คนงาน’ จริงที่ทำให้ข้อความถูกส่งไปถึง client
broadcast_queue เป็น สายพาน ที่ส่งงานกระจายข้อความจากส่วน Router → ไปยัง worker
main server เป็น producer (เรียก push)
worker เป็น consumer (เรียก pop)
```cpp
void broadcaster_worker()
{
    std::cout << "Broadcaster thread " << std::this_thread::get_id() << " started." << std::endl;

    while (true)
    {
        // 1) รอหยิบงานจากคิว (บล็อกจนมีคน push)
        BroadcastTask task = broadcast_queue.pop();
        std::string room_to_broadcast;

        // 2) ถ้างานระบุห้องมาแล้ว ก็ใช้เลย → กรณี system event
        if (!task.target_room.empty())
        {
            room_to_broadcast = task.target_room;
        }
        else
        {
            // 3) ถ้าไม่ระบุห้อง แปลว่าเป็น SAY ปกติ → ต้องหาเองว่าคนนี้อยู่ห้องไหน
            ReadLock lock(registry_lock);
            for (const auto &pair : room_members)
            {
                const auto &members = pair.second;
                if (std::find(members.begin(), members.end(), task.sender_name) != members.end())
                {
                    room_to_broadcast = pair.first;
                    break;
                }
            }
        }

        // ถ้าหาห้องไม่เจอ ก็ข้ามงานนี้ไป
        if (room_to_broadcast.empty())
            continue;

        // 4) อ่านรายชื่อสมาชิกในห้อง แล้วส่งให้ทีละคน
        ReadLock lock(registry_lock);

        // เผื่อห้องโดนลบ/แก้ไขระหว่างรอ
        if (room_members.find(room_to_broadcast) == room_members.end())
            continue;

        for (const auto &member : room_members.at(room_to_broadcast))
        {
            // ไม่ต้องส่งกลับให้คนส่งเอง
            if (member == task.sender_name)
                continue;

            std::string qname = "/client_" + member;
            // เปิดคิวของ client ปลายทางแบบ non-blocking
            mqd_t client_q = mq_open(qname.c_str(), O_WRONLY | O_NONBLOCK);

            if (client_q != -1)
            {
                // ส่งข้อความ
                if (mq_send(client_q, task.message_payload.c_str(), task.message_payload.size() + 1, 0) == -1)
                {
                    // ถ้าปลายทางคิวเต็ม อันนี้เป็นแบบ best-effort
                    if (errno == EAGAIN)
                    {
                        // จะเพิ่ม logic แจ้งกลับคนส่งก็ได้ (ต้นฉบับคุณมีในเวอร์ชันก่อนหน้า)
                    }
                }
                mq_close(client_q);
            }
            else
            {
                std::cerr << "Broadcaster mq_open failed for " << member << std::endl;
            }
        }
    }
}


```
   6.4 การสร้าง Broadcaster Pool ใน main: ใน main() ของเซิร์ฟเวอร์จะสร้าง worker หลายตัวตั้งแต่เริ่มเลย เพื่อให้รองรับงานกระจายข้อความหลาย ๆ งานพร้อมกัน

   ตรงนี้คือจุดที่ยืนยันว่าเซิร์ฟเวอร์ “ไม่ได้ส่งข้อความด้วยเธรดเดียว” แต่แบ่งงานออกไปให้หลายเธรดไปทำพร้อมกัน ทำให้เวลามีห้องใหญ่ ๆ หรือมี system event มาพร้อมกันหลายอัน เซิร์ฟเวอร์ยังตอบสนองต่อคำสั่งใหม่ได้ทัน เพราะ main ไม่ได้รอส่งข้อความเอง

```cpp
int main()
{
    pthread_rwlock_init(&registry_lock, NULL);

    // Create broadcaster pool
    int num_broadcasters = 32; // สร้าง worker 32 ตัวไว้เลย
    for (int i = 0; i < num_broadcasters; ++i)
        std::thread(broadcaster_worker).detach();
    std::cout << "Broadcaster pool (size=" << num_broadcasters << ") started." << std::endl;

    // ... โค้ดส่วนอื่น (heartbeat, รวมคิว /server, loop รับข้อความ) ...
}



```

สรุปพาร์ท Broadcast System 

Broadcast System ในโปรแกรมเซิร์ฟเวอร์นี้เป็นกลไกที่แยก “การรับคำสั่งจาก client” ออกจาก “การส่งข้อความไปยังทุกคนในห้อง” อย่างชัดเจน โดยใช้คิวงานส่วนกลาง (broadcast_queue) เป็นตัวกลางระหว่างส่วน Router/Handler กับกลุ่มเธรด Broadcaster วิธีนี้ทำให้ตัวเซิร์ฟเวอร์ไม่จำเป็นต้องส่งข้อความหาทุก client ด้วยตัวเองในเธรดหลัก ซึ่งจะทำให้เกิดการบล็อก แต่จะเปลี่ยนข้อความทุกชนิด (ทั้งข้อความที่ผู้ใช้พิมพ์ และข้อความระบบ เช่น JOIN/LEAVE/QUIT) ให้กลายเป็นงาน (task) แล้วให้เธรดเบื้องหลังเป็นผู้กระจายออกไปแทน ส่งผลให้ระบบสามารถรองรับผู้ใช้หลายคนและหลายห้องได้ดีขึ้น และยังขยายจำนวน worker ได้ง่ายในอนาคต

---

พาร์ทที่ 7: main() – การบูตเซิร์ฟเวอร์และลูปรับข้อความ : Main System หรือที่เรียกว่า Router System เป็นศูนย์กลางการควบคุมการทำงานของเซิร์ฟเวอร์ทั้งหมด มีหน้าที่รับข้อความจาก client ทุกคน ประมวลผลคำสั่ง และส่งต่อไปยังส่วนที่เกี่ยวข้อง เช่น Handler หรือ Broadcast System โดยทำงานแบบต่อเนื่องตลอดอายุการรันของโปรแกรม

กระบวนการเริ่มต้นของ main()

เมื่อโปรแกรมเริ่มทำงาน ฟังก์ชัน main() จะทำหน้าที่เตรียมและเปิดระบบดังนี้

1.กำหนดกลไกการล็อกข้อมูลร่วมกัน (RW Lock) ใช้ pthread_rwlock_init() เพื่อป้องกันการเข้าถึงข้อมูลห้อง (room_members) พร้อมกันจากหลาย thread

2.สร้างกลุ่ม Broadcaster Workers สร้างเธรดหลายตัวด้วย std::thread(broadcaster_worker).detach() เพื่องานกระจายข้อความ (ดู พาร์ท Broadcast System) ซึ่งช่วยให้การส่งข้อความไม่ไปรบกวน main thread

3.เริ่มต้น Heartbeat Cleaner Thread เธรดนี้เรียก heartbeat_cleaner() คอยตรวจสอบ client ที่ไม่ส่งสัญญาณ PING ภายในเวลาที่กำหนด หากหมดเวลา จะลบออกจากระบบโดยอัตโนมัติ

4.เปิด Message Queue หลักของ Server ใช้ mq_open("/server", O_CREAT | O_RDWR, 0644, &attr) เพื่อสร้างและเปิด queue กลางชื่อ /server เป็นช่องทางรับข้อความจากทุก client

5.เข้าสู่ลูปรับข้อความ (Main Loop) ใช้ mq_receive() รับข้อความจาก queue ของ client ทีละข้อความ จากนั้นตรวจสอบ prefix ของข้อความว่าเป็นคำสั่งใด เช่น REGISTER:, JOIN:, SAY: หรือ QUIT: แล้วส่งต่อไปยังฟังก์ชัน handler ที่เกี่ยวข้อง

6.เชื่อมโยงกับระบบ Broadcast เมื่อ handler ใด (เช่น handle_join, handle_leave, handle_quit, หรือ handle_say) ตรวจพบเหตุการณ์ที่ต้องแจ้งต่อผู้อื่น จะสร้าง BroadcastTask ใหม่และใส่งานลงใน broadcast_queue เพื่อให้ broadcaster_worker() ทำการกระจายข้อความต่อ

ดังนั้น Main System ทำหน้าที่ "สร้างและส่งงาน" ในขณะที่ Broadcast System ทำหน้าที่ "รับงานและกระจายต่อ"
สองระบบนี้ทำงานควบคู่กันอย่างไม่บล็อกกัน (Asynchronous Communication) จึงทำให้เซิร์ฟเวอร์สามารถตอบสนองต่อผู้ใช้หลายคนพร้อมกันได้อย่างราบรื่น

```cpp
int main()
{
    // 1) เตรียม rwlock
    pthread_rwlock_init(&registry_lock, NULL);

    // 2) สร้าง worker กระจายข้อความหลาย ๆ ตัว
    int num_broadcasters = 32; // จำนวน thread กระจายข้อความ
    for (int i = 0; i < num_broadcasters; ++i)
        std::thread(broadcaster_worker).detach();
    std::cout << "Broadcaster pool (size=" << num_broadcasters << ") started." << std::endl;

    // 3) สร้าง thread สำหรับล้าง client ที่ไม่ส่ง heartbeat
    std::thread(heartbeat_cleaner).detach();
    std::cout << "Heartbeat cleaner thread started." << std::endl;

    // 4) ตั้งค่า message queue ของ server
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 1000;   // รองรับโหลดเยอะหน่อย
    attr.mq_msgsize = 1024;
    attr.mq_curmsgs = 0;

    // 5) เปิดคิว /server ไว้รอ client ส่งข้อความมา
    mqd_t server_q = mq_open("/server", O_CREAT | O_RDWR, 0644, &attr);
    if (server_q == -1)
    {
        perror("mq_open not complete");
        return 1;
    }
    std::cout << "Server opened." << std::endl;

    // 6) ลูปรอรับข้อความจากทุก client
    char buf[1024];
    while (true)
    {
        ssize_t n = mq_receive(server_q, buf, sizeof(buf), nullptr);
        if (n > 0)
        {
            buf[n] = '\0';
            std::string msg(buf);

            // route ไปยัง handler ตาม prefix
            if      (msg.rfind("REGISTER:", 0) == 0) handle_register(msg);
            else if (msg.rfind("JOIN:",     0) == 0) handle_join(msg);
            else if (msg.rfind("SAY:",      0) == 0) handle_say(msg);
            else if (msg.rfind("DM:",       0) == 0) handle_dm(msg);
            else if (msg.rfind("WHO:",      0) == 0) handle_who(msg);
            else if (msg.rfind("LEAVE:",    0) == 0) handle_leave(msg);
            else if (msg.rfind("QUIT:",     0) == 0) handle_quit(msg);
            else if (msg.rfind("PING:",     0) == 0) handle_ping(msg);
            else std::cout << "Unknown message: " << msg << std::endl;
        }
    }

    // (โค้ดนี้จริง ๆ ไม่ถึงตรงนี้เพราะ while(true) แต่เขียนไว้ให้ครบ)
    mq_close(server_q);
    mq_unlink("/server");
    return 0;
}


```
---
สรุป 

เซิร์ฟเวอร์ตัวนี้แบ่งชัดเจนเป็น 3 ชั้นการทำงาน:

ชั้นรับคำสั่ง (Input / Router): main อ่านจาก /server แล้วแยกไป handler

ชั้นจัดการสถานะ (Handlers): ปรับข้อมูลห้อง, รายชื่อ client, สร้าง broadcast task

ชั้นกระจายข้อความ (Broadcast Pool): worker หลายตัวดึงงานจากคิวแล้วส่งไปยังคิวของ client แต่ละคน

และมี ระบบเฝ้าระวัง (Heartbeat Cleaner) แยกอีกตัวเพื่อเคลียร์ client ที่หลุด ทำให้สถานะบน server ไม่รก

---
Client
---

ระบบ Client ทำหน้าที่เป็นส่วนติดต่อของผู้ใช้ (User Interface) สำหรับเชื่อมต่อและสื่อสารกับเซิร์ฟเวอร์ โดยทำหน้าที่รับ–ส่งข้อมูลผ่าน POSIX Message Queue ซึ่งเป็นกลไกสื่อสารระหว่างโปรเซส 

ฝั่ง Client จะรับคำสั่งจากผู้ใช้ แปลงคำสั่งให้อยู่ในรูปข้อความที่ Server เข้าใจ เช่น "JOIN:...", "SAY:...", "DM:..." แล้วส่งขึ้นไปยังคิวกลางของเซิร์ฟเวอร์ชื่อ /server
ในขณะเดียวกัน Client จะเปิดคิวรับข้อความของตนเอง (เช่น /client_alice) เพื่อรอรับข้อความที่ถูกส่งกลับมาจากเซิร์ฟเวอร์ เช่น ข้อความในห้อง, ข้อความส่วนตัว (DM), หรือข้อความระบบ (system message)

 โดยที่โปรแกรมนี้ Client ถูกแบ่งออกเป็น 3 ส่วนหลัก ดังนี้

พาร์ทที่ 1:เธรดเบื้องหลัง: Heartbeat และ Listener

  1.1 Heartbeat Thread : เซิร์ฟเวอร์มีระบบ “ล้าง client ที่หายไปนาน” อยู่แล้ว มันจะดูว่า client คนไหนไม่ส่ง PING:<name> ภายในเวลาที่กำหนดก็จะถือว่าหลุด แล้วไปลบจากห้องและ broadcast บอกคนอื่น เพราะฉะนั้น client ทุกตัวต้องมีตัวส่ง heartbeat เป็นระยะ
      ใช้ O_NONBLOCK เพราะไม่อยากให้ heartbeat ไปค้างถ้า server คิวเต็มชั่วคราว ,ใช้การเปิด/ปิดคิวรอบต่อรอบ เพราะง่ายและชัดเจน
  ```cpp
//  HEARTBEAT SYSTEM
//  ส่ง "PING:<client_name>" ไปหา server ทุก ๆ 10 วินาที

void heartbeat_sender(const std::string &client_name, const std::string &server_qname)
{
    const std::string ping_msg = "PING:" + client_name;

    while (keep_running)
    {
        // เว้นช่วงก่อนส่งรอบถัดไป
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (!keep_running)
            break;  // ถ้า main สั่งปิด ก็ออกทันที

        // เปิดคิวของ server ในโหมดเขียน (non-blocking เผื่อ server ไม่พร้อม)
        mqd_t server_q = mq_open(server_qname.c_str(), O_WRONLY | O_NONBLOCK);
        if (server_q == -1) {
            // เปิดไม่ได้ก็ปล่อยรอบนี้ไป
            continue;
        }

        // ส่ง ping เพื่อบอกว่าคลายเอนต์นี้ยังออนไลน์อยู่
        mq_send(server_q, ping_msg.c_str(), ping_msg.size() + 1, 0);
        mq_close(server_q);
    }
}

  ```
  1.2 Listener Thread : ฟังก์ชัน listen_queue() ทำหน้าที่เป็น "หู" ของ client เซิร์ฟเวอร์จะส่งข้อความถึง อีกเธรดหนึ่งมีหน้าที่ “นั่งรอ” ว่า server จะส่งอะไรมาให้เราไหม  อาจเป็นข้อความจากคนอื่นในห้อง, system message, ผลลัพธ์ของคำสั่ง WHO, หรือข้อความ         error เวลา DM ไม่เจอคน อยากให้เธรดนี้หยุดได้เมื่อผู้ใช้ QUIT จึงใช้ mq_timedreceive() ไม่ใช้ mq_receive() แบบบล็อกยาว ๆ เพื่อให้ทุก ๆ 2 วินาที เธรดจะมีโอกาสเช็ค keep_running แล้วออกจาก loop ได้

```cpp
void listen_queue(const std::string &qname)
{
    struct mq_attr attr;
    attr.mq_flags   = 0;
    attr.mq_maxmsg  = 10;
    attr.mq_msgsize = 1024;
    attr.mq_curmsgs = 0;

    // เปิดคิวของ client เพื่อรอข้อความที่ server ส่งมา
    mqd_t client_q = mq_open(qname.c_str(), O_CREAT | O_RDONLY, 0644, &attr);

    char buf[1024];
    while (keep_running)
    {
        // ตั้ง timeout 2 วิ เพื่อไม่ให้บล็อกตลอด
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;

        // รอรับข้อความจาก server สูงสุด 2 วิ
        ssize_t n = mq_timedreceive(client_q, buf, sizeof(buf), nullptr, &ts);
        if (n > 0)
        {
            buf[n] = '\0';
            // ขึ้นบรรทัดใหม่แล้วแสดงข้อความจาก server
            std::cout << "\n"
                      << ANSI_COLOR_YELLOW << buf << ANSI_COLOR_RESET << "\n"
                      << ANSI_COLOR_GREEN << "> " << ANSI_COLOR_RESET << std::flush;
        }
        // ถ้า timeout ก็วนใหม่ แล้วไปเช็ค keep_running ที่ while
    }

    mq_close(client_q);
}


```
พาร์ทที่ 2: main: การเริ่มต้น, วงวนคำสั่ง, และปิดโปรแกรม : ส่วนหลักสุดของ client ที่บอกลำดับ “ชีวิตของ client” ตั้งแต่เริ่มจนจบ

```cpp
int main(int argc, char *argv[])
{
    // 1) ต้องมีชื่อ client ตอนรัน เช่น ./client alice
    if (argc < 2)
    {
        std::cerr << "+++++ USAGE: ./<client_file> <client_name> +++++" << std::endl;
        return 1;
    }

    // 2) ตั้งชื่อสำคัญ ๆ ของ client
    std::string client_name  = argv[1];                  // ชื่อเรา
    std::string client_qname = "/client_" + client_name; // คิวที่ server จะใช้ส่งมาหาเรา
    std::string current_room = "";                       // จะอัปเดตตอน JOIN

    // 3) สตาร์ท thread ฟังข้อความจาก server ก่อนเลย
    std::thread listener_thread(listen_queue, client_qname);

    // 4) เปิดคิวของ server สำหรับ "ส่ง" ข้อความขึ้นไป
    mqd_t server_q = mq_open("/server", O_WRONLY);

    // 5) ส่ง REGISTER ให้ server รู้ว่าเรามีคิวชื่ออะไร
    std::string reg_msg = "REGISTER:" + client_qname;
    mq_send(server_q, reg_msg.c_str(), reg_msg.size() + 1, 0);

    // 6) สตาร์ท heartbeat thread ให้ ping ไปเรื่อย ๆ
    std::thread heartbeat_thread(heartbeat_sender, client_name, "/server");

    // 7) แสดงเมนู + บอกชื่อที่ register แล้ว
    system("clear");
    std::cout << std::endl;
    innitial_commands();
    std::cout << std::endl;
    std::cout << "REGISTERED AS " << client_name << std::endl;
    std::cout << std::endl;
    std::cout << ANSI_COLOR_GREEN << "> " << ANSI_COLOR_RESET << std::flush;

    // 8) วงวนหลัก: รอผู้ใช้พิมพ์คำสั่ง
    std::string msg;
    while (std::getline(std::cin, msg))
    {
        // ---- SAY ----
        if (msg.rfind("SAY:", 0) == 0)
        {
            std::string send_msg = "SAY:[" + client_name + "]: " + msg.substr(4);
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }
        // ---- JOIN ----
        else if (msg.rfind("JOIN:", 0) == 0)
        {
            current_room = msg.substr(5);
            std::string send_msg = "JOIN:" + client_name + ": " + current_room;
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
            system("clear");
            std::cout << "Joined #" << current_room << " successfully" << std::endl;
        }
        // ---- DM ----
        else if (msg.rfind("DM:", 0) == 0)
        {
            size_t pos = msg.find(':', 3);
            if (pos == std::string::npos)
            {
                std::cout << "Invalid DM format. Use: DM:<target>:<message>\n> ";
                continue;
            }

            std::string target = msg.substr(3, pos - 3);
            std::string text   = msg.substr(pos + 1);
            std::string send_msg = "DM:" + client_name + ":" + target + ":" + text;
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }
        // ---- WHO ----
        else if (msg.rfind("WHO:", 0) == 0)
        {
            std::string send_msg = "WHO:" + client_name + ">" + current_room;
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }
        // ---- LEAVE ----
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
        // ---- QUIT ----
        else if (msg.rfind("QUIT:", 0) == 0)
        {
            // ถ้าไม่ยืนยันก็วนต่อ
            if (!confirm_action("quit the server"))
            {
                std::cout << "> ";
                continue;
            }

            // ถ้ายังอยู่ในห้องอยู่ ให้แจ้ง LEAVE ก่อนค่อย QUIT
            if (!current_room.empty())
            {
                std::string payload_leave = "LEAVE:" + client_name;
                mq_send(server_q, payload_leave.c_str(), payload_leave.size() + 1, 0);
                std::cout << "You left room before quitting." << std::endl;
                current_room.clear();
            }

            // ตอนนี้ค่อยส่ง QUIT
            std::string payload_quit = "QUIT:" + client_name;
            mq_send(server_q, payload_quit.c_str(), payload_quit.size() + 1, 0);

            // บอก thread อื่น ๆ ให้หยุด
            keep_running = false;
            break;
        }
        // ---- Unknown ----
        else if (!msg.empty())
        {
            std::cout << "Command not found." << std::endl;
        }

        // แสดง prompt ใหม่
        std::cout << ANSI_COLOR_GREEN << "> " << ANSI_COLOR_RESET << std::flush;
    }

    // 9) cleanup ตอนจบโปรแกรม
    mq_close(server_q);                 // ปิดคิวส่ง
    listener_thread.join();             // รอ thread ฟังข้อความจบ
    heartbeat_thread.join();            // รอ thread heartbeat จบ
    mq_unlink(client_qname.c_str());    // ลบคิวของเราออกจากระบบ
    return 0;
}


```
พาร์ทที่ 3: วงวนหลักโต้ตอบกับผู้ใช้ + cleanup
เอ่าน input จาก std::cin ทีละบรรทัด วิเคราะห์ prefix เพื่อดูว่าเป็นคำสั่งประเภทไหน แล้วแพ็กข้อความตามโปรโตคอล ก่อนส่งไป server ผ่านคิว /server

คำสั่งหลัก:

SAY: พูดกับทั้งห้อง

JOIN: เข้าห้องใหม่

DM: ส่งข้อความส่วนตัว

WHO: ขอรายชื่อในห้อง

LEAVE: ออกจากห้องปัจจุบัน (มี confirm)

QUIT: ออกจากระบบทั้งหมด (มี confirm + เลือกรูปแบบประกาศ)

เมื่อ QUIT: จะตั้ง keep_running = false เพื่อหยุดทั้ง heartbeat thread และ listen thread ส่ง "QUIT:<...>" ให้ server

break ออกจากลูป แล้ว clean up ทุกอย่าง (join threads, ปิดและลบคิว)

```cpp
       std::string msg;
    while (std::getline(std::cin, msg))
    {
        // --------------------
        // คำสั่งพูดในห้อง (SAY)
        // รูปแบบ input:  SAY:hello guys
        // ส่งไป server:  "SAY:[alice]: hello guys"
        // server จะไป broadcast ให้สมาชิกห้องเดียวกัน (ยกเว้นตัวเรา)
        if (msg.rfind("SAY:", 0) == 0)
        {
            std::string send_msg = "SAY:[" + client_name + "]: " + msg.substr(4);
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }

        // --------------------
        // คำสั่งเข้าห้อง/ย้ายห้อง (JOIN)
        // input: JOIN:room1
        // ส่ง:   "JOIN:alice: room1"
        // server:
        //   - เอา alice ออกจากห้องเดิม (ถ้ามี)
        //   - ใส่ alice เข้า room1
        //   - broadcast [SYSTEM]: alice has joined #room1
        else if (msg.rfind("JOIN:", 0) == 0)
        {
            current_room = msg.substr(5); // ตัด "JOIN:" เหลือชื่อห้อง
            std::string send_msg = "JOIN:" + client_name + ": " + current_room;
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);

            // เคลียร์จอ + แจ้งว่า join สำเร็จ เพื่อ UX ที่ชัดเจน
            system("clear");
            std::cout << "Joined   # " + current_room + "   successfully" << std::endl;
        }

        // --------------------
        // คำสั่ง DM (Direct Message)
        // input: DM:bob:hey man
        // ส่ง:   "DM:alice:bob:hey man"
        // server จะลองเปิดคิวของ bob แล้วส่ง
        // ถ้าไม่เจอ จะส่ง error message กลับมาหา alice
        else if (msg.rfind("DM:", 0) == 0)
        {
            // แยก target กับข้อความ
            // รูปแบบต้องเป็น DM:<target>:<message>
            size_t pos = msg.find(':', 3);
            if (pos == std::string::npos)
            {
                std::cout << "Invalid DM format. Use: DM:<target>:<message>\n> ";
                continue;
            }

            std::string target = msg.substr(3, pos - 3); // คนปลายทาง
            std::string text   = msg.substr(pos + 1);    // เนื้อความ DM

            std::string send_msg =
                "DM:" + client_name + ":" + target + ":" + text;
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }

        // --------------------
        // คำสั่ง WHO
        // input: WHO:
        // ส่ง:   "WHO:alice>room1"
        // server จะส่งรายชื่อสมาชิกของ room1 กลับเข้าคิว /client_alice
        else if (msg.rfind("WHO:", 0) == 0)
        {
            std::string send_msg = "WHO:" + client_name + ">" + current_room;
            mq_send(server_q, send_msg.c_str(), send_msg.size() + 1, 0);
        }

        // --------------------
        // คำสั่ง LEAVE
        // input: LEAVE:
        // กระบวนการ:
        //   - ถ้าไม่ได้อยู่ห้องไหน ก็แจ้งเลย
        //   - ถ้าอยู่ ขอ confirm ก่อน
        //   - ถ้ายืนยัน ส่ง "LEAVE:<name>" ให้ server
        // server จะลบเราจาก room_members และ broadcast ว่าเราออกห้องแล้ว
        else if (msg.rfind("LEAVE:", 0) == 0)
        {
            if (current_room.empty()) {
                std::cout << "You are not in any room." << std::endl;
            } else if (confirm_action("leave room #" + current_room)) {
                std::string room_that_left = current_room;
                current_room = ""; // เราไม่อยู่ห้องใดในฝั่ง client แล้ว

                std::string payload = "LEAVE:" + client_name;
                mq_send(server_q, payload.c_str(), payload.size() + 1, 0);

                std::cout << "You left room #" << room_that_left << std::endl;
            }
        }

        // --------------------
        // คำสั่ง QUIT
        // input: QUIT:
        // ขั้นตอน:
        //   1. ยืนยันก่อน
        //   2. ถ้ามี current_room อยู่:
        //         ให้ user เลือกว่าจะประกาศออกแบบไหน:
        //         - โหมด 1: "QUIT:<name>"
        //           (broadcast แค่ 'quit')
        //         - โหมด 2: "QUIT:<name>:leave_then_quit"
        //           (broadcast ว่าออกห้องก่อน แล้วตามด้วย quit)
        //      ถ้าไม่ได้อยู่ในห้องแล้ว ส่งแค่ "QUIT:<name>"
        //   3. set keep_running=false → สั่งทุก thread หยุด
        //   4. ส่ง payload ไป server
        //   5. break ออกจาก while เพื่อไป cleanup
        else if (msg.rfind("QUIT:", 0) == 0)
        {
            if (confirm_action("quit the server")) {
                
                std::string payload;

                if (!current_room.empty()) {
                    // ยังอยู่ในห้อง → ถามว่าประกาศแบบไหน
                    std::cout << ANSI_COLOR_YELLOW
                              << "How do you want to quit?"
                              << ANSI_COLOR_RESET << std::endl;
                    std::cout << "1. Just quit (Announce 'quit' to room)" << std::endl;
                    std::cout << "2. Announce 'leave' then 'quit' (Two messages)" << std::endl;
                    std::cout << ANSI_COLOR_GREEN
                              << "Enter choice (1 or 2): "
                              << ANSI_COLOR_RESET << std::flush;
                    
                    std::string choice_line;
                    std::getline(std::cin, choice_line);
                    
                    if (choice_line == "2") {
                        // โหมด leave_then_quit
                        payload = "QUIT:" + client_name + ":leave_then_quit";
                    } else {
                        // โหมดปกติ
                        payload = "QUIT:" + client_name;
                    }
                } else {
                    // ไม่ได้อยู่ห้องไหน → ส่ง quit ปกติ
                    payload = "QUIT:" + client_name;
                }

                // บอก thread ทั้งหมดว่าเรากำลังจะปิด
                keep_running = false;
                current_room = ""; 
            
                // ส่งคำสั่ง QUIT ขึ้นไปบอก server
                mq_send(server_q, payload.c_str(), payload.size() + 1, 0);

                // ออกจากลูปหลัก
                break;
            }
        }

        // --------------------
        // ถ้าไม่ match คำสั่งไหนเลย
        else {
            std::cout << "command not found" << std::endl;
        }

        // แสดง prompt ใหม่หลังจากประมวลผลคำสั่งเสร็จ
        std::cout << ANSI_COLOR_GREEN << "> " << ANSI_COLOR_RESET << std::flush;
    }

    // ---------- Cleanup เมื่อออกจากลูปหลัก (เช่น หลัง QUIT) ----------

    // ปิด queue ฝั่ง server ที่เราใช้ส่งคำสั่ง
    mq_close(server_q);

    // รอ thread ฟังข้อความ + thread heartbeat จบก่อนจะ kill โปรเซส
    t.join();
    pinger.join();

    // ลบ message queue ส่วนตัวของ client ออกจากระบบ (เหมือนบอกว่าเรา offline ถาวรแล้ว)
    mq_unlink(client_qname.c_str());

    return 0;
}


```

สรุปรายงานภาพรวมของโปรแกรม Client

โปรแกรม Client มีหน้าที่เป็นตัวกลางระหว่างผู้ใช้งานกับ Server เพื่อสื่อสารกันผ่านระบบ POSIX Message Queue (MQ) โดยผู้ใช้สามารถพิมพ์คำสั่งต่าง ๆ เพื่อเข้าห้อง พูดคุย ส่งข้อความส่วนตัว ดูรายชื่อสมาชิก หรือออกจากระบบได้ โปรแกรมนี้ใช้หลักการทำงานแบบ หลายเธรด (Multithreading) เพื่อให้สามารถทำงานหลายอย่างพร้อมกัน ได้แก่

Main Thread สำหรับรับคำสั่งจากผู้ใช้และส่งไปยัง Server

Listener Thread สำหรับรอรับข้อความจาก Server แล้วแสดงผลทันที

Heartbeat Thread สำหรับส่งสัญญาณ “PING” ไปยัง Server เป็นระยะ เพื่อบอกว่าผู้ใช้งานยังออนไลน์อยู่

การทำงานทั้งหมดถูกควบคุมด้วยตัวแปร keep_running เพื่อให้ทุกเธรดหยุดพร้อมกันเมื่อผู้ใช้สั่ง QUIT:.
ด้วยโครงสร้างแบบนี้ โปรแกรม Client สามารถทำงานแบบ Asynchronous Communication คือ การสื่อสารที่ไม่ต้องรอให้แต่ละฝ่ายทำงานเสร็จทีละขั้นตอนก่อนถึงจะดำเนินต่อได้






---
How to complie
---

โปรแกรมนี้เป็นระบบแชตแบบ Client–Server ที่ใช้ POSIX Message Queue (mqueue.h) เพื่อสื่อสารระหว่างโปรเซส โดยใช้ multi-threading ในฝั่ง client และ server เพื่อแยกงาน เช่น การส่ง heartbeat, การ broadcast ข้อความ และการตรวจสอบ client ที่หลุดการเชื่อมต่อ

---
การคอมไพล์และใช้งาน (Compilation & Usage)
คอมไพล์ Server:
```cpp
g++ -std=c++11 server.cpp -o server -pthread -lrt
```
คอมไพล์ Client:
```cpp
g++ -std=c++11 client.cpp -o client -pthread -lrt
```
---
การรันโปรแกรม (Running)

รัน Server: ใน Terminal แรก:
```cpp
./server
```
รัน Client: เปิด Terminal ใหม่ (กี่อันก็ได้) แล้วรันไคลเอนต์ โดยระบุชื่อผู้ใช้ใน Terminal ที่สอง:
```cpp
./client alice
```
Terminal ที่สาม:
```cpp
./client bob
```
---
Example Session
```cpp
$ ./server
server opened
Broadcaster pool (size=4) started.
Heartbeat cleaner thread started.
/client_alice has join the server!
/client_bob has join the server!

=== Room Members ===
room1: alice 
room2: 
room3: 
====================
[SYSTEM]: alice has joined #room1
bob has joined #room1

=== Room Members ===
room1: alice bob 
room2: 
room3: 
====================
[SYSTEM]: bob has joined #room1
[alice]: Hi bob!
bob → alice : Hello alice
[SYSTEM]: alice has left #room1
bob has quit the server.
```
Terminal 2 (Client 'alice'):
```cpp
$ ./client alice
Registered as alice
> JOIN:room1
Joined   # room1   successfully
> 
[SYSTEM]: bob has joined #room1
> SAY:Hi bob!
> 
[DM from bob]: Hello alice
> LEAVE:
Are you sure you want to leave room #room1? (y/n): y
You left room #room1
>
```
Terminal 3 (Client 'bob'):
```cpp
$ ./client bob
Registered as bob
> JOIN:room1
Joined   # room1   successfully
> 
[SYSTEM]: alice has joined #room1
> 
[alice]: Hi bob!
> DM:alice:Hello alice
> 
[SYSTEM]: alice has left #room1
> QUIT:
Are you sure you want to quit the server? (y/n): y
How do you want to quit?
1. Just quit (Announce 'quit' to room)
2. Announce 'leave' then 'quit' (Two messages)
Enter choice (1 or 2): 1
$
```
คำสั่งที่ Client ใช้ได้ (Available Client Commands)
JOIN:<room_name>: เข้าร่วมห้องแชท (เช่น JOIN:room1)

SAY:<message>: ส่งข้อความไปยังทุกคนในห้องปัจจุบัน (เช่น SAY:Hello everyone!)

DM:<target_name>:<message>: ส่งข้อความส่วนตัว (DM) ไปยังผู้ใช้อื่น (เช่น DM:bob:Hi bob)

WHO:<room_name>: ดูรายชื่อสมาชิกทั้งหมดในห้องที่ระบุ (เช่น WHO:room1)

LEAVE:: ออกจากห้องปัจจุบัน

QUIT:: ออกจากเซิร์ฟเวอร์

---

Performance
---



