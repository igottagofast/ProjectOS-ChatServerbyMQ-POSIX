Server
---

ระบบ Server ทำหน้าที่เป็นศูนย์กลางในการสื่อสารระหว่างผู้ใช้ (client) หลายคน โดยจะรับคำสั่งจากผู้ใช้แต่ละราย ผ่าน POSIX Message Queue และจัดการประมวลผลตามคำสั่งที่ได้รับ เช่น การเข้าระบบ (REGISTER), การเข้าห้อง (JOIN), การส่งข้อความ (SAY), การออกจากห้อง (LEAVE), การออกจากระบบ (QUIT) รวมถึงการส่งสัญญาณตรวจสอบสถานะ (PING)

โครงสร้างการทำงานของ Server ในโปรเเกรมนี้แยกหน้าที่การทำงานออกเป็น 2 ส่วนหลัก (2 พาร์ท) เพื่อให้แต่ละส่วนรับผิดชอบงานของตัวเองได้อย่างชัดเจน และสามารถทำงานแบบขนานได้ (concurrent processing) โดยแต่ละส่วนมีรายละเอียดดังนี้

พาร์ทที่ 1: Broadcast System
เป็นส่วนที่รับผิดชอบการ “กระจายข้อความ” จากผู้ใช้หนึ่งคนไปยังผู้ใช้อื่นในห้องแชทเดียวกัน โดยใช้กลไกของคิวกลางและเธรดหลายตัว (thread pool) เพื่อให้การส่งข้อความเกิดขึ้นอย่างรวดเร็วและไม่รบกวนเธรดหลักของเซิร์ฟเวอร์

พาร์ทที่ 2: Main / Router System
เป็นส่วนควบคุมหลักของเซิร์ฟเวอร์ ทำหน้าที่รับคำสั่งจากผู้ใช้ทั้งหมด วิเคราะห์ประเภทของคำสั่ง และเรียกใช้ฟังก์ชันที่เหมาะสม เช่น การจัดการผู้ใช้ในห้อง การส่งข้อความ การตรวจสอบสถานะการเชื่อมต่อ และการสร้างงานกระจายข้อความส่งต่อไปยังส่วน Broadcast

การแบ่งระบบออกเป็น 2 พาร์ทนี้ ช่วยให้โครงสร้างการทำงานของ Server มีความยืดหยุ่น และสามารถรองรับผู้ใช้จำนวนมากพร้อมกันได้ โดยไม่ทำให้ระบบล่าช้าหรือค้างระหว่างการส่งข้อมูล เนื่องจากส่วน Main System จะทำหน้าที่รับคำสั่งและควบคุมการทำงานหลัก ส่วน Broadcast System จะรับหน้าที่ส่งข้อความแบบขนาน จึงทำให้การประมวลผลและการสื่อสารเกิดขึ้นได้พร้อมกัน

---
องค์ประกอบหลักของสถานะระบบ:
```cpp
// เก็บสมาชิกปัจจุบันในแต่ละห้อง
std::map<std::string, std::vector<std::string>> room_members = {
    {"room1", {}},
    {"room2", {}},
    {"room3", {}}
};

// mutex แบบอ่าน/เขียนสำหรับป้องกันการแก้ข้อมูลห้องพร้อมกันหลายเธรด
pthread_rwlock_t registry_lock;

// เก็บรายชื่อคิวของ client ทั้งหมด (เช่น "/client_alice", "/client_bob")
std::vector<std::string> client_queues;

// เก็บ timestamp ล่าสุดที่ client ส่ง heartbeat (PING)
std::map<std::string, std::chrono::steady_clock::time_point> client_heartbeats;
std::mutex heartbeat_mutex;

```
---
พาร์ท Broadcast System เป็นส่วนที่ทำหน้าที่กระจายข้อความจากผู้ใช้หนึ่งคนไปยังผู้ใช้คนอื่น ๆ ภายในห้องแชทเดียวกัน (chat room) ให้เกิดขึ้นอย่างรวดเร็วและไม่รบกวนการทำงานของส่วนหลักของเซิร์ฟเวอร์

กระบวนการทำงานเริ่มจาก เมื่อมีข้อความหรือเหตุการณ์ใหม่เกิดขึ้น เช่น ผู้ใช้ส่งข้อความ (SAY) เข้าห้อง หรือมีการเข้าห้อง (JOIN) / ออกจากห้อง (LEAVE) ฟังก์ชันในพาร์ท Main System จะสร้างวัตถุ (object) ชื่อ BroadcastTask ขึ้นมา ซึ่งภายในจะบันทึกข้อมูลสำคัญ ได้แก่ message_payload คือเนื้อหาของข้อความที่ต้องส่ง, sender_name คือชื่อผู้ส่ง, target_room คือชื่อห้องที่จะส่งข้อความไป

จากนั้นงานนี้จะถูกส่งเข้ามาเก็บในคิวกลางชื่อว่า broadcast_queue ซึ่งเป็นคิวชนิดพิเศษที่ออกแบบให้ ปลอดภัยต่อการเข้าถึงพร้อมกันหลายเธรด (thread-safe) โดยคิวนี้มีฟังก์ชันหลักคือ push() สำหรับใส่งานเข้า และ pop() สำหรับดึงงานออกมาใช้งาน

เมื่อมีงานใหม่ถูกส่งเข้าคิวแล้ว เธรดในส่วน Broadcaster ซึ่งทำงานอยู่ตลอดเวลาจะเป็นผู้มาดึงงานนั้นออกไปประมวลผล ฟังก์ชันที่ทำหน้าที่นี้คือ broadcaster_worker()

โครงสร้างงาน Broadcast:
```cpp
struct BroadcastTask
{
    std::string message_payload; // เนื้อความที่จะส่ง เช่น "[alice]: hi" หรือ "[SYSTEM]: bob has left #room1"
    std::string sender_name;     // ใช้เพื่อไม่ส่งข้อความซ้ำกลับไปที่คนส่งเอง
    std::string target_room;     // ห้องเป้าหมาย (อาจว่างถ้าเป็นข้อความพูดปกติ แล้วต้องเดาห้องจาก sender)
};

```
ระบบ Broadcast (การกระจายข้อความแบบขนาน)

ข้อความทุกอันในห้อง (รวมถึงประกาศระบบ เช่น "Alice has joined") จะไม่ถูกส่งออกโดย main thread โดยตรง
แต่ถูกแปลงเป็น “งาน” (task) แล้วใส่ลงในคิวกลาง จากนั้นมี worker threads หลายตัวทำหน้าที่กระจายข้อความออกไปยัง clientที่เกี่ยวข้อง

คิวงาน Thread-safe:

```cpp
// TaskQueue<T> คือคิวงานที่ออกแบบมาให้ใช้ข้ามเธรดได้อย่างปลอดภัย (thread-safe)
// ใช้ pattern แบบ Producer-Consumer:
//   - ฝั่ง Producer เรียก push() ใส่งานเข้าคิว
//   - ฝั่ง Consumer (เช่น worker thread) เรียก pop() ดึงงานออกมาทำ
// จุดสำคัญคือเราป้องกัน race ด้วย mutex และใช้ condition_variable
// เพื่อให้ consumer "รออย่างถูกต้อง" จนกว่าจะมีงาน ไม่ใช่ลูปเช็คตลอด (ไม่ busy-wait)

template <typename T>
class TaskQueue
{
public:
    // push(item):
    //   - ใช้โดยฝั่ง Producer
    //   - ใส่งานใหม่ (item) เข้าไปในคิว
    //   - จากนั้นปลุก 1 เธรดที่กำลังรอ pop() อยู่ ให้ตื่นมาทำงาน
    void push(T item) {
        std::unique_lock<std::mutex> lock(mtx); // ล็อกคิวกันหลายเธรดเขียนพร้อมกัน
        queue.push(item);                       // ใส่ task เข้าไปท้ายคิว
        lock.unlock();                          // ปลดล็อกก่อนปลุก เพื่อให้ consumer จับล็อกต่อได้เร็ว
        cv.notify_one();                        // ปลุกหนึ่งเธรดที่กำลังรอ pop()
    }

    // pop():
    //   - ใช้โดยฝั่ง Consumer (เช่น broadcaster_worker)
    //   - ถ้าไม่มีงานในคิว ให้รอ (block) จนกว่าจะมีใคร push งานเข้ามา
    //   - เมื่อมีงานแล้ว จะดึงงานตัวแรกออก (FIFO)
    T pop() {
        std::unique_lock<std::mutex> lock(mtx); // จับล็อกก่อนแตะต้องคิว
        // รอจนกว่าจะมีอย่างน้อย 1 งานในคิว
        // cv.wait(...) จะปลดล็อก mtx ชั่วคราวระหว่างรอ
        // แล้วกลับมาล็อกให้ใหม่อัตโนมัติเมื่อถูกปลุก
        cv.wait(lock, [this]{ return !queue.empty(); });

        T item = queue.front(); // หยิบงานตัวหน้าสุดของคิว (FIFO)
        queue.pop();            // เอางานนั้นออกจากคิว
        return item;            // ส่งงานให้คนเรียกไปประมวลผลต่อ
    }

private:
    std::queue<T> queue;            // โครงสร้างคิวจริง ๆ เก็บงานตามลำดับ
    std::mutex mtx;                 // ป้องกันไม่ให้หลายเธรดแก้ queue พร้อมกัน
    std::condition_variable cv;     // ตัวช่วยให้ consumer รอแบบ block จนกว่าจะมีงาน
};

// ในระบบเซิร์ฟเวอร์เราใช้ TaskQueue กับชนิด BroadcastTask
// เพื่อให้ main thread "ใส่งานกระจายข้อความ" ลงคิว
// แล้วให้ broadcaster_worker (หลายเธรด) มาดึงงานจากคิวนี้ไปส่งห้องแชท
TaskQueue<BroadcastTask> broadcast_queue;

```
broadcast_queue เป็น สายพาน ที่ส่งงานกระจายข้อความจากส่วน Router → ไปยัง worker
main server เป็น producer (เรียก push)
worker เป็น consumer (เรียก pop)

Worker ที่กระจายข้อความ:
```cpp
void broadcaster_worker()
{
    std::cout << "Broadcaster thread " << std::this_thread::get_id() << " started." << std::endl;

    while (true)
    {
        BroadcastTask task = broadcast_queue.pop();
        std::string room_to_broadcast;

        // ถ้ามี target_room ระบุไว้ (เช่น system message JOIN/LEAVE)
        if (!task.target_room.empty()) {
            room_to_broadcast = task.target_room;
        } else {
            // ถ้าเป็น SAY ปกติ → เดาว่าคนนี้อยู่ห้องไหน
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

            if (room_members.find(room_to_broadcast) == room_members.end())
                continue;

            for (const auto &member : room_members.at(room_to_broadcast)) {

                if (member == task.sender_name)
                    continue; // ไม่ต้องส่งกลับให้ตัวส่งเอง

                std::string qname = "/client_" + member;
                mqd_t client_q = mq_open(qname.c_str(), O_WRONLY | O_NONBLOCK);

                if (client_q != -1) {
                    if (mq_send(client_q, task.message_payload.c_str(), task.message_payload.size() + 1, 0) == -1) {
                        if (errno == EAGAIN) {
                            // ถ้าคิวปลายทางเต็ม → แจ้งเตือนกลับไปหาคนส่ง
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

```

การสร้าง worker หลายตัว (thread pool) 
ใน main():

```cpp
int num_broadcasters = 4; // ค่า default = 4 thread
const char *env_p = std::getenv("NUM_BROADCASTERS");
if (env_p != nullptr) {
    num_broadcasters = std::atoi(env_p);
    if (num_broadcasters <= 0) num_broadcasters = 1;
}
const int NUM_BROADCASTERS = num_broadcasters;

std::vector<std::thread> workers;
for (int i = 0; i < NUM_BROADCASTERS; ++i)
{
    workers.emplace_back(broadcaster_worker);
    workers.back().detach();
}
std::cout << "Broadcaster pool (size=" << NUM_BROADCASTERS << ") started." << std::endl;


```
เซิร์ฟเวอร์ไม่พึ่ง thread เดียวในการส่งข้อความออกไป มันสร้าง “Broadcaster Pool” หลายเธรด เพื่อรองรับโหลดสูง จำนวนเธรดปรับได้ผ่านตัวแปร environment NUM_BROADCASTERS

การจัดการห้องและสมาชิก (JOIN / LEAVE / WHO)
- JOIN (ย้ายเข้าห้อง)
```cpp
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

ระบบ Heartbeat และการตรวจจับ client ที่หลุด

ปัญหาในระบบ chat จริง: client อาจ “ปิดโปรแกรมไปเฉย ๆ” โดยไม่ส่ง QUIT
ถ้าไม่จัดการ เซิร์ฟเวอร์จะคิดว่าคนนั้นยังอยู่ในห้อง ซึ่งในโปรเเกรมนี้ป้องกันด้วย heartbeat ทำหน้าที่ อัปเดตเวลา “ลูกค้าคนนี้ยังหายใจอยู่” ทุกครั้งที่ client ส่ง PING:<name> มา
```cpp
void handle_ping(const std::string &msg)
{
    std::string client_name = msg.substr(5); // "PING:<name>"
    std::lock_guard<std::mutex> lock(heartbeat_mutex);
    client_heartbeats[client_name] = std::chrono::steady_clock::now();
}


```

heartbeat_cleaner() — ฝั่งล้างศพ รันเป็น thread แยกตลอดเวลา 

ทุก 15 วินาที เซิร์ฟเวอร์จะเช็ก client ทุกคน

ถ้าไม่ส่ง heartbeat ภายใน 30 วินาที จะถือว่า “ตายแล้ว”

ระบบจะเรียก handle_quit() แทนลูกค้า เพื่อเคลียร์ห้อง, broadcast ว่าคนนั้นออก, และลบ heartbeat

:เซิร์ฟเวอร์สามารถจัดการ client ที่ disconnect ไปเงียบ ๆ โดยอัตโนมัติ → ไม่มีผีค้างห้อง
```cpp
void heartbeat_cleaner()
{
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::seconds(15));

        std::vector<std::string> dead_clients;
        auto now = std::chrono::steady_clock::now();
        const int TIMEOUT_SECONDS = 30;

        {
            std::lock_guard<std::mutex> lock(heartbeat_mutex);
            for (const auto &pair : client_heartbeats)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - pair.second).count();
                if (elapsed > TIMEOUT_SECONDS)
                {
                    dead_clients.push_back(pair.first);
                }
            }
        }

        for (const std::string &client_name : dead_clients)
        {
            std::cout << "[SYSTEM] Heartbeat timeout for " << client_name << ". Cleaning up." << std::endl;

            // บังคับเหมือน user ส่ง QUIT เอง
            handle_quit("QUIT:" + client_name);

            // ส่งซ้ำเข้า /server (optional notify)
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

Main Server Loop (Router หลัก)

พาร์ท Main System หรือก็คือส่วนของ Router System เป็นส่วนหลักของเซิร์ฟเวอร์ที่ทำหน้าที่ “ควบคุมการไหลของข้อมูล” ระหว่าง client แต่ละคน และเป็นศูนย์กลางในการรับคำสั่งจากผู้ใช้ทุกคนในระบบ

เมื่อเริ่มทำงาน ฟังก์ชัน main() จะเป็นจุดเริ่มต้นของโปรแกรมฝั่งเซิร์ฟเวอร์ โดย กำหนดค่าและเริ่มใช้งานล็อกกลาง (pthread_rwlock_init) เพื่อป้องกันการเข้าถึงข้อมูล room_members พร้อมกันจากหลายเธรด จากนั้น สร้างกลุ่มเธรด Broadcaster (เรียกใช้ broadcaster_worker()) เพื่อทำหน้าที่กระจายข้อความจากคิว broadcast_queue ซึ่งเป็นการเชื่อมโยงกับพาร์ท Broadcast System เเละทำการสร้างเธรดตรวจสอบการเชื่อมต่อ (Heartbeat Cleaner) โดยเรียก heartbeat_cleaner() เพื่อคอยตรวจสอบ client ที่ไม่ส่งสัญญาณ PING เกินเวลาที่กำหนด และสั่งลบออกจากระบบอัตโนมัติ เเละเปิด Message Queue /server ด้วย mq_open() เพื่อรอรับข้อความจาก client ตลอดเวลาการทำงานของเซิร์ฟเวอร์ หลังจากขั้นตอนการเตรียมระบบเสร็จแล้ว ฟังก์ชัน main() จะเข้าสู่ลูปหลักในการรอรับข้อความจาก client ด้วย mq_receive()

ภายในลูปหลักของ main() เซิร์ฟเวอร์จะรับข้อความจาก message queue /server จากนั้นตรวจสอบว่าข้อความที่ได้รับขึ้นต้นด้วยคำสั่งประเภทใด (โดยดูจาก prefix ของ string เช่น "REGISTER:", "JOIN:", "SAY:" ) แล้วเรียกใช้ฟังก์ชันจัดการ (handler)

เมื่อฟังก์ชันใด ๆ ในส่วน Main ตรวจพบว่ามีข้อความหรือเหตุการณ์ที่ต้องกระจายให้ผู้อื่นทราบ (เช่นจาก handle_join, handle_leave, handle_quit หรือข้อความ SAY: ใน main() เอง) ฟังก์ชันนั้นจะสร้าง BroadcastTask แล้วใส่งานลงใน broadcast_queue เพื่อให้ broadcaster_worker() จากพาร์ท Broadcast System เป็นผู้จัดการส่งข้อความต่อ

ดังนั้น ส่วน Main จะทำหน้าที่ “สร้างและส่งงาน” ในขณะที่ส่วน Broadcast จะทำหน้าที่ “รับงานและกระจายต่อ”
```cpp
int main()
{
    pthread_rwlock_init(&registry_lock, NULL);

    // 1) เริ่ม Broadcaster Worker Pool
    int num_broadcasters = 4;
    const char *env_p = std::getenv("NUM_BROADCASTERS");
    if (env_p != nullptr)
    {
        num_broadcasters = std::atoi(env_p);
        if (num_broadcasters <= 0) num_broadcasters = 1;
    }
    const int NUM_BROADCASTERS = num_broadcasters;

    std::vector<std::thread> workers;
    for (int i = 0; i < NUM_BROADCASTERS; ++i)
    {
        workers.emplace_back(broadcaster_worker);
        workers.back().detach();
    }
    std::cout << "Broadcaster pool (size=" << NUM_BROADCASTERS << ") started." << std::endl;

    // 2) เริ่ม heartbeat cleaner
    std::thread cleaner_thread(heartbeat_cleaner);
    cleaner_thread.detach();
    std::cout << "Heartbeat cleaner thread started." << std::endl;

    // 3) เปิด message queue กลางของ server
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

    // 4) วนรับคำสั่งตลอดเวลา
    char buf[1024];
    while (true)
    {
        ssize_t n = mq_receive(server_q, buf, sizeof(buf), nullptr);
        if (n > 0)
        {
            buf[n] = '\0';
            std::string msg(buf);

            if      (msg.rfind("REGISTER:", 0) == 0) { handle_register(msg); }
            else if (msg.rfind("JOIN:",     0) == 0) { handle_join(msg); }
            else if (msg.rfind("SAY:",      0) == 0) {
                std::string payload = msg.substr(4);
                size_t start = payload.find('[');
                size_t end   = payload.find(']');
                std::string sender = payload.substr(start + 1, end - start - 1);

                BroadcastTask task;
                task.message_payload = payload;
                task.sender_name = sender;
                task.target_room = ""; // ให้ broadcaster_worker หาเองว่าห้องไหน
                broadcast_queue.push(task);
            }
            else if (msg.rfind("DM:",       0) == 0) { handle_dm(msg); }
            else if (msg.rfind("WHO:",      0) == 0) { handle_who(msg); }
            else if (msg.rfind("LEAVE:",    0) == 0) { handle_leave(msg); }
            else if (msg.rfind("QUIT:",     0) == 0) { handle_quit(msg); }
            else if (msg.rfind("PING:",     0) == 0) { handle_ping(msg); }
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
Client
---

ระบบ Client ทำหน้าที่เป็นส่วนติดต่อของผู้ใช้ (User Interface) สำหรับเชื่อมต่อและสื่อสารกับเซิร์ฟเวอร์ โดยทำหน้าที่รับ–ส่งข้อมูลผ่าน POSIX Message Queue ซึ่งเป็นกลไกสื่อสารระหว่างโปรเซส 

ฝั่ง Client จะรับคำสั่งจากผู้ใช้ แปลงคำสั่งให้อยู่ในรูปข้อความที่ Server เข้าใจ เช่น "JOIN:...", "SAY:...", "DM:..." แล้วส่งขึ้นไปยังคิวกลางของเซิร์ฟเวอร์ชื่อ /server
ในขณะเดียวกัน Client จะเปิดคิวรับข้อความของตนเอง (เช่น /client_alice) เพื่อรอรับข้อความที่ถูกส่งกลับมาจากเซิร์ฟเวอร์ เช่น ข้อความในห้อง, ข้อความส่วนตัว (DM), หรือข้อความระบบ (system message)

 โดยที่โปรแกรมนี้ Client ถูกแบ่งออกเป็น 4 ส่วนหลัก ดังนี้

พาร์ทที่ 1 : การเริ่มต้นและการเชื่อมต่อกับเซิร์ฟเวอร์

พาร์ทที่ 2 : การรับและแสดงข้อความจากเซิร์ฟเวอร์

พาร์ทที่ 3 : การส่งคำสั่งและข้อความจากผู้ใช้ไปยังเซิร์ฟเวอร์

พาร์ทที่ 4 : ระบบ Heartbeat และการออกจากระบบ

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

#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_RESET   "\x1b[0m"

// ฟังก์ชัน heartbeat จะอธิบายในพาร์ทที่ 4
void heartbeat_sender(std::string client_name, std::string server_qname);
void listen_queue(const std::string &qname);

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "USAGE!!!    --->    ./client [client_name]" << std::endl;
        return 1;
    }

    std::string client_name = argv[1];
    std::string client_qname = "/client_" + client_name;
    std::string current_room = "";

    // สร้าง thread สำหรับฟังข้อความจาก server
    std::thread t(listen_queue, client_qname);

    // เปิดคิวของ server เพื่อส่งข้อมูล
    mqd_t server_q = mq_open("/server", O_WRONLY);
    std::string reg_msg = "REGISTER:" + client_qname;
    mq_send(server_q, reg_msg.c_str(), reg_msg.size() + 1, 0);

    std::cout << "Registered as " << client_name << "\n"
              << ANSI_COLOR_GREEN << "> " << ANSI_COLOR_RESET << std::flush;

    // สร้าง thread สำหรับ heartbeat (จะอธิบายในพาร์ท 4)
    std::thread pinger(heartbeat_sender, client_name, "/server");
```
พาร์ทที่ 1: การเริ่มต้นและการเชื่อมต่อกับเซิร์ฟเวอร์ (Initialization & Registration)

พาร์ทนี้เป็นขั้นตอนเริ่มต้นของ client ตั้งแต่การ include library, การตั้งค่าตัวแปร,
การประกาศ thread, และการลงทะเบียนตัวเองกับเซิร์ฟเวอร์ผ่าน message queue /server.

```cpp
void listen_queue(const std::string &qname)
{
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = 1024;
    attr.mq_curmsgs = 0;

    // เปิดคิวของ client เพื่อรับข้อความ
    mqd_t client_q = mq_open(qname.c_str(), O_CREAT | O_RDONLY, 0644, &attr);
    if (client_q == -1)
    {
        perror("mq_open client not complete.");
        return;
    }

    char buf[1024];
    while (keep_running)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;

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

```
พาร์ทที่ 2: การรับและแสดงข้อความจากเซิร์ฟเวอร์ (Listening / Incoming Messages)

พาร์ทนี้คือ “หู” ของ client ที่ฟังทุกข้อความจากเซิร์ฟเวอร์
และแสดงผลบนหน้าจอทันทีโดยใช้สีเพื่อแยกข้อความระบบกับ prompt พิมพ์คำสั่ง.

```cpp
    // ฟังก์ชันยืนยันก่อนทำการ quit หรือ leave
    auto confirm_action = [](const std::string &action) -> bool {
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
        else if (msg.rfind("QUIT:", 0) == 0)
        {
            if (confirm_action("quit the server")) {
                std::string payload;

                if (!current_room.empty()) {
                    std::cout << ANSI_COLOR_YELLOW << "How do you want to quit?" << ANSI_COLOR_RESET << std::endl;
                    std::cout << "1. Just quit (Announce 'quit' to room)" << std::endl;
                    std::cout << "2. Announce 'leave' then 'quit' (Two messages)" << std::endl;
                    std::cout << ANSI_COLOR_GREEN << "Enter choice (1 or 2): " << ANSI_COLOR_RESET << std::flush;

                    std::string choice_line;
                    std::getline(std::cin, choice_line);

                    if (choice_line == "2")
                        payload = "QUIT:" + client_name + ":leave_then_quit";
                    else
                        payload = "QUIT:" + client_name;
                } 
                else payload = "QUIT:" + client_name;

                keep_running = false;
                current_room = "";

                mq_send(server_q, payload.c_str(), payload.size() + 1, 0);
                break;
            }
        }
        else {
            std::cout << "command not found" << std::endl;
        }

        std::cout << ANSI_COLOR_GREEN << "> " << ANSI_COLOR_RESET << std::flush;
    }

```
พาร์ทที่ 3: การส่งคำสั่งจากผู้ใช้ไปยังเซิร์ฟเวอร์ (Outgoing Commands / User Input)

พาร์ทนี้คือ “ปาก” ของ client ที่รับคำสั่งจากผู้ใช้
แล้วแปลงเป็นข้อความ protocol เพื่อส่งไปยังเซิร์ฟเวอร์ผ่าน message queue /server.


```cpp
// ฟังก์ชัน Heartbeat: ส่ง PING เป็นระยะเพื่อบอก server ว่ายังเชื่อมต่ออยู่
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

        mq_send(server_q, ping_msg.c_str(), ping_msg.size() + 1, 0);
        mq_close(server_q);
    }
}

// ปิดระบบอย่างปลอดภัยหลังจากออกจาก loop หลัก
    mq_close(server_q);
    t.join();
    pinger.join();
    mq_unlink(client_qname.c_str());
    return 0;
}


```
พาร์ทที่ 4: ระบบ Heartbeat และการออกจากระบบอย่างปลอดภัย (Heartbeat & Graceful Shutdown)

พาร์ทนี้เป็นระบบตรวจสอบการเชื่อมต่อ และขั้นตอนออกจากระบบให้สมบูรณ์
เพื่อให้ server รู้ว่า client ยังออนไลน์หรือออกจากระบบแล้วจริง ๆ


---
How to complie
---


Performance
---



