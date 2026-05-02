# Campus Exam Invigilator Scheduling System
## EGC 301P — Operating Systems Lab Mini Project Report

---

## 1. Problem Statement

University examinations require careful coordination of venues and invigilators. Managing this manually is error-prone, especially when multiple faculty members try to book the same venue simultaneously, or when several invigilators attempt to confirm the same slot at the same time.

The **Campus Exam Invigilator Scheduling System** is a client-server application built in C that automates this coordination. It allows faculty to book exam venues and assign invigilators, allows invigilators to view and confirm their assignments, and provides an admin interface for managing the system. The core challenge — and the core demonstration of OS concepts — lies in handling all of this safely and correctly when multiple clients connect and operate concurrently.

---

## 2. System Overview

The system follows a classic client-server architecture over TCP sockets. The server is a long-running process that maintains all data in memory (backed by binary files on disk), and serves multiple clients simultaneously using POSIX threads. The client is an interactive terminal program.

### Roles and Their Capabilities

| Role | Capabilities |
|---|---|
| **Admin** | Add/remove venues, invigilators, faculty; view all venues and bookings |
| **Faculty** | View available venues, book a venue, assign invigilators (direct or broadcast) |
| **Invigilator** | View assignments, confirm an assignment, change password |

### Data Entities

- **Invigilators** and **Faculty** — identified by username and password
- **Venues** — have a unique ID, name, and capacity
- **Bookings** — link a faculty member to a venue for a specific date and time slot
- **Assignments** — link an invigilator to a booking; can be Pending, Confirmed, or Cancelled

---

## 3. Implementation of Required OS Concepts

### 3.1 Role-Based Authorization

Three distinct roles are implemented with strictly separated access controls.

When a client connects, the first message it sends is a role selector (1 = Admin, 2 = Faculty, 3 = Invigilator). The server routes the connection to the appropriate handler (`process_admin`, `process_faculty`, `process_invigilator`), each of which has a completely separate set of allowed operations.

**Admin authentication** uses a hardcoded password (`ADMIN_PASSWORD = "admin123"`). The server compares the received password with `strcmp` and returns `-1` to the client on failure, terminating the session immediately.

**Faculty and Invigilator authentication** use a username-password lookup against the in-memory arrays. The server returns the found index (≥ 0) on success or `-1` on failure. Only after a successful authentication response does the client proceed to send a menu choice.

This means no cross-role operations are possible — a faculty client cannot invoke invigilator operations, and neither can access admin functions. All sensitive operations (adding users, removing venues, etc.) are gated exclusively to the admin handler.

---

### 3.2 File Locking

All persistent data is stored in five binary files:

```
invigilators.bin   faculty.bin   venues.bin   bookings.bin   assignments.bin
```

Safe concurrent file access is implemented using **POSIX `fcntl` advisory locking** via the `flock_set` and `flock_release` helper functions:

```c
static void flock_set(int fd, short type)
{
    struct flock fl = { type, SEEK_SET, 0, 0, 0 };
    fcntl(fd, F_SETLKW, &fl);   // blocking lock
}
static void flock_release(int fd)
{
    struct flock fl = { F_UNLCK, SEEK_SET, 0, 0, 0 };
    fcntl(fd, F_SETLK, &fl);
}
```

The `SAVE` and `LOAD` macros use these helpers to acquire a **write lock** (`F_WRLCK`) before writing and a **read lock** (`F_RDLCK`) before reading. The lock covers the entire file region (offset 0, length 0 = whole file). `F_SETLKW` is used for writes (blocking wait), ensuring no two threads corrupt a file simultaneously.

This protects the persistence layer independently of the in-memory mutex, so that even if the saver thread and a hypothetical external reader access the file at the same time, the data remains consistent.

---

### 3.3 Concurrency Control

The server accepts clients in an infinite loop and spawns a new **POSIX thread** for each connection:

```c
pthread_create(&tid, NULL, process_client, ca);
pthread_detach(tid);
```

All shared in-memory data (the arrays of invigilators, faculty, venues, bookings, assignments, and the ID counters) is protected by a single global **mutex**:

```c
pthread_mutex_t g_lock;
```

Every handler function acquires this mutex before touching any shared data and releases it before returning or sending a response. For example, in `process_faculty`:

```c
pthread_mutex_lock(&g_lock);
// ... all reads and writes to shared arrays ...
pthread_mutex_unlock(&g_lock);
```

Two additional background threads also run from startup:

- **`saver_thread`** — wakes every 10 seconds, acquires `g_lock`, and calls `save_all()` to flush in-memory state to disk.
- **`pipe_reader_thread`** — reads assignment IDs from the IPC pipe and logs them to the console (described under IPC below).

Both background threads also respect `g_lock`, ensuring they cannot interfere with client-handling threads.

---

### 3.4 Data Consistency

Two specific race conditions were identified and explicitly addressed:

**Race Condition 1 — Venue Double Booking**

If two faculty clients simultaneously attempt to book the same venue for the same time slot, without synchronization both could pass the availability check before either commits the booking.

This is prevented by holding `g_lock` across the entire check-and-insert sequence in the faculty booking handler:

```c
pthread_mutex_lock(&g_lock);

if (venue_is_booked(inp.venue_id, inp.date, inp.start_hour, inp.end_hour)) {
    int v = -2; write(ns, &v, sizeof(int));
    pthread_mutex_unlock(&g_lock); return;
}
// ... create booking ...
bookings[num_bookings++] = b;
pthread_mutex_unlock(&g_lock);
```

The `venue_is_booked` function checks for any time overlap (not just exact matches) using the condition `sh < b->end_hour && eh > b->start_hour`. Since the mutex is held throughout, no second thread can enter the same critical section concurrently.

**Race Condition 2 — Invigilator Over-Confirmation**

When a faculty broadcasts a request to all invigilators (and more invigilators than needed respond), multiple confirmations could race to increment `confirmed_count` past the required number.

This is handled inside the confirm handler under `g_lock`:

```c
if (b->confirmed_count >= b->invigilators_needed) {
    a->status = STATUS_CANCELLED;
    v = -5; break;   // slot already full — cancel this one
}
a->status = STATUS_CONFIRMED;
b->confirmed_count++;
```

The first N invigilators to confirm succeed; any subsequent confirmation finds `confirmed_count >= invigilators_needed` and has their assignment auto-cancelled. The invigilator receives a clear message: *"Sorry, this slot is already fully staffed."*

---

### 3.5 Socket Programming

The system uses **TCP stream sockets** (`AF_INET`, `SOCK_STREAM`) for all client-server communication.

**Server setup:**

```c
int sd = socket(AF_INET, SOCK_STREAM, 0);
setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
bind(sd, (struct sockaddr*)&server, sizeof(server));
listen(sd, 10);
// accept loop spawns a thread per connection
```

**Client setup:**

```c
int sd = socket(AF_INET, SOCK_STREAM, 0);
inet_pton(AF_INET, SERVER_IP, &server.sin_addr);
connect(sd, (struct sockaddr*)&server, sizeof(server));
```

Communication is entirely binary — structs are serialized with `#pragma pack(1)` to eliminate padding, ensuring the same byte layout on both sides. This means no serialization library is needed; a plain `write(sd, &struct, sizeof(struct))` / `read(sd, &struct, sizeof(struct))` exchange is sufficient and efficient.

The server listens on port **8080** and can handle multiple simultaneous clients because each accepted connection runs in its own thread.

---

### 3.6 Inter-Process Communication (IPC) — Unnamed Pipe

An **unnamed pipe** is used for intra-process IPC between the client-handling threads and the `pipe_reader_thread`.

```c
int pipe_fd[2];
pipe(pipe_fd);   // created in main()
```

Whenever an assignment is created — either by direct assignment (Faculty option 4) or broadcast (Faculty option 5) — the assignment ID is written into the write end of the pipe:

```c
write(pipe_fd[1], &aid, sizeof(long long));
```

The dedicated `pipe_reader_thread` continuously reads from the read end:

```c
void *pipe_reader_thread(void *arg) {
    long long aid;
    while (1)
        if (read(pipe_fd[0], &aid, sizeof(long long)) > 0)
            printf("[IPC] Assignment request created — ID: %lld\n", aid);
    return NULL;
}
```

This demonstrates a clean producer-consumer IPC pattern: multiple producer threads (client handlers) write events into the pipe, and a single consumer thread reads and processes them. The pipe's kernel-level atomicity for small writes (≤ `PIPE_BUF`) ensures that individual `long long` writes are not interleaved.

---

## 4. Architecture Diagram

```
                        ┌─────────────────────────────┐
                        │         SERVER PROCESS      │
                        │                             │
  Client 1 ──TCP──►  Thread 1 ──┐                     │
  Client 2 ──TCP──►  Thread 2 ──┼──► g_lock (mutex) ──┼──► Shared Memory
  Client 3 ──TCP──►  Thread 3 ──┘         │           │    (arrays)
                        │                 │           │       │
                        │   Saver Thread ─┘           │       │
                        │   (every 10s)               │       ▼
                        │                             │  Binary Files
                        │   Pipe Write ◄── Thread N   │  (fcntl locks)
                        │       │                     │
                        │   Pipe Reader Thread        │
                        │   (logs to console)         │
                        └─────────────────────────────┘
```

---

## 5. Key Design Decisions

**Binary protocol with `#pragma pack(1)`** — Using packed structs for wire communication avoids the overhead of text parsing and ensures byte-for-byte compatibility between client and server on the same machine. Both files define identical struct layouts.

**Soft deletion with `is_active` flag** — Rather than physically removing records and shifting arrays, entities are marked inactive (`is_active = 0`). This preserves array indices and avoids pointer invalidation across threads, which would be difficult to manage safely.

**Periodic save rather than write-through** — The saver thread flushes data every 10 seconds instead of on every write operation. This reduces I/O overhead significantly under load. The trade-off is a potential loss of up to 10 seconds of data on a crash, which is acceptable for a scheduling system.

**Single global mutex** — A single `g_lock` covering all shared state simplifies correctness reasoning at the cost of some parallelism (only one client handler runs its business logic at a time). For the scale of this system (campus use, tens of concurrent users) this is an appropriate trade-off.

---

## 6. Error Handling and Edge Cases

| Scenario | Handling |
|---|---|
| Invalid role at login | Server closes connection silently |
| Wrong admin password | Server returns `-1`; client prints error and exits |
| Invalid faculty/invigilator credentials | Server returns `-1`; client prints error and exits |
| Booking a non-existent venue | Returns `-1` to client |
| Double-booking a venue | `venue_is_booked` check under mutex; returns `-2` |
| Assigning an invigilator with a time clash | `invigilator_has_clash` check; returns `-4` |
| Duplicate invigilator assignment | Duplicate check in `create_assignment`; returns `-5` |
| Over-confirmation of a fully staffed slot | Auto-cancels late confirmation; returns `-5` |
| Removing a venue cascades to bookings | All bookings for that venue set `is_active = 0` |
| Removing a faculty cascades to bookings | All bookings for that faculty set `is_active = 0` |
| Removing an invigilator cascades to assignments | All assignments for that invigilator set `is_active = 0` |

---

## 7. How to Compile and Run

```bash
# Compile server
gcc -o server server.c -lpthread

# Compile client
gcc -o client client.c

# Run server (in one terminal)
./server

# Run one or more clients (in separate terminals)
./client
```

The server must be started before any clients connect. Data files are created automatically in the working directory on the first write.

---

## 8. Challenges Faced and Solutions

**Challenge 1 — Ensuring atomicity of check-then-act operations**
Simply checking availability before booking is not enough if two threads can interleave their checks. The solution was to hold `g_lock` across the entire check-and-commit sequence, not just the commit.

**Challenge 2 — Broadcast over-staffing**
When a faculty broadcasts to all invigilators, the system cannot know in advance which N will confirm first. The solution was to allow all invigilators to receive a pending assignment, but enforce the cap atomically at confirmation time, auto-cancelling any excess.

**Challenge 3 — Safe concurrent file I/O**
Multiple threads calling `save_all` simultaneously (e.g., if the saver thread and a direct save overlap) could corrupt files. `fcntl` write locks ensure only one writer holds the lock at a time, with others blocking until it is released.

**Challenge 4 — Packed struct alignment**
Without `#pragma pack(1)`, the compiler inserts padding between struct fields, causing the client and server to interpret the same byte stream differently. Applying `#pragma pack(1)` to all wire structs in both files resolves this completely.

---

## 9. Summary of OS Concepts Demonstrated

| Concept | Where Implemented |
|---|---|
| Role-Based Authorization | `process_admin`, `process_faculty`, `process_invigilator` — separate handlers, credential checks |
| File Locking | `flock_set` / `flock_release` via `fcntl F_RDLCK / F_WRLCK` in `SAVE` / `LOAD` macros |
| Concurrency Control | `pthread_create` per client; `pthread_mutex_t g_lock` protecting all shared data |
| Data Consistency | Mutex-protected check-and-commit for booking; cap-enforced confirmation for invigilator slots |
| Socket Programming | TCP client-server on port 8080; binary struct protocol |
| IPC (Unnamed Pipe) | `pipe(pipe_fd)` — client threads write assignment IDs; `pipe_reader_thread` reads and logs them |
