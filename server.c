#define _XOPEN_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#define PORT 8080
#define MAX_USERS 100
#define MAX_VENUES 200
#define MAX_BOOKINGS 1000
#define MAX_ASSIGNMENTS 5000
#define SAVE_INTERVAL 10
#define ADMIN_PASSWORD "admin123"

// assignment status 
#define STATUS_PENDING 0
#define STATUS_CONFIRMED 1
#define STATUS_CANCELLED 2

// file names 
const char *inv_file = "invigilators.bin";
const char *fac_file = "faculty.bin";
const char *venue_file = "venues.bin";
const char *book_file = "bookings.bin";
const char *assign_file = "assignments.bin";

// global mutex + IPC pipe 
pthread_mutex_t g_lock;
int pipe_fd[2];

#pragma pack(1)
typedef struct { char username[30]; char password[30]; } Invigilator;
typedef struct { char username[30]; char password[30]; } Faculty;

typedef struct {
    long long venue_id;
    char name[50];
    int capacity;
    int is_active;
} Venue;

typedef struct {
    long long booking_id;
    long long venue_id;
    char faculty_username[30];
    char venue_name[50];
    char date[20];
    int start_hour;
    int end_hour;
    int invigilators_needed;
    int confirmed_count;
    int is_active;
} Booking;

typedef struct {
    long long assignment_id;
    long long booking_id;
    char invigilator_username[30];
    char venue_name[50];
    char date[20];
    int start_hour;
    int end_hour;
    int status;
    int is_active;
} Assignment;

// input structs (client → server)
typedef struct {
    long long venue_id;
    char date[20];
    int start_hour;
    int end_hour;
    int invigilators_needed;
} BookingInput;

// view structs (server → client)
typedef struct { long long venue_id; char name[50]; int capacity; } VenueView;

typedef struct {
    long long booking_id;
    long long venue_id;
    char venue_name[50];
    char faculty_username[30];
    char date[20];
    int start_hour;
    int end_hour;
    int invigilators_needed;
    int confirmed_count;
} BookingView;

typedef struct {
    long long assignment_id;
    long long booking_id;
    char venue_name[50];
    char date[20];
    int start_hour;
    int end_hour;
    int status;
} AssignmentView;
#pragma pack()

// in-memory arrays
Invigilator invigilators[MAX_USERS];
Faculty faculties[MAX_USERS];
Venue venues[MAX_VENUES];
Booking bookings[MAX_BOOKINGS];
Assignment assignments[MAX_ASSIGNMENTS];

int num_invigilators = 0;
int num_faculties = 0;
int num_venues = 0;
int num_bookings = 0;
int num_assignments = 0;
long long next_venue_id = 1;
long long next_booking_id = 1;
long long next_assign_id = 1;

// FILE I/O helpers with fcntl locking
static void flock_set(int fd, short type)
{
    struct flock fl = { type, SEEK_SET, 0, 0, 0 };
    fcntl(fd, F_SETLKW, &fl);
}
static void flock_release(int fd)
{
    struct flock fl = { F_UNLCK, SEEK_SET, 0, 0, 0 };
    fcntl(fd, F_SETLK, &fl);
}

#define SAVE(file, count_var, array, type)                          \
    do {                                                            \
        int _fd = open(file, O_WRONLY|O_CREAT|O_TRUNC, 0644);      \
        if (_fd >= 0) {                                             \
            flock_set(_fd, F_WRLCK);                               \
            write(_fd, &count_var, sizeof(int));                   \
            write(_fd, array, count_var * sizeof(type));           \
            flock_release(_fd); close(_fd);                        \
        }                                                           \
    } while(0)

#define SAVE_WITH_ID(file, count_var, id_var, array, type)         \
    do {                                                            \
        int _fd = open(file, O_WRONLY|O_CREAT|O_TRUNC, 0644);      \
        if (_fd >= 0) {                                             \
            flock_set(_fd, F_WRLCK);                               \
            write(_fd, &count_var, sizeof(int));                   \
            write(_fd, &id_var, sizeof(long long));                \
            write(_fd, array, count_var * sizeof(type));           \
            flock_release(_fd); close(_fd);                        \
        }                                                           \
    } while(0)

#define LOAD(file, count_var, array, type)                          \
    do {                                                            \
        int _fd = open(file, O_RDONLY);                             \
        if (_fd >= 0) {                                             \
            flock_set(_fd, F_RDLCK);                               \
            read(_fd, &count_var, sizeof(int));                    \
            read(_fd, array, count_var * sizeof(type));            \
            flock_release(_fd); close(_fd);                        \
        }                                                           \
    } while(0)

#define LOAD_WITH_ID(file, count_var, id_var, array, type)         \
    do {                                                            \
        int _fd = open(file, O_RDONLY);                             \
        if (_fd >= 0) {                                             \
            flock_set(_fd, F_RDLCK);                               \
            read(_fd, &count_var, sizeof(int));                    \
            read(_fd, &id_var, sizeof(long long));                 \
            read(_fd, array, count_var * sizeof(type));            \
            flock_release(_fd); close(_fd);                        \
        }                                                           \
    } while(0)

void save_all()
{
    SAVE (inv_file, num_invigilators, invigilators, Invigilator);
    SAVE (fac_file, num_faculties, faculties, Faculty);
    SAVE_WITH_ID (venue_file, num_venues, next_venue_id, venues, Venue);
    SAVE_WITH_ID (book_file,  num_bookings, next_booking_id, bookings, Booking);
    SAVE_WITH_ID (assign_file,num_assignments, next_assign_id, assignments, Assignment);
}

void load_all()
{
    LOAD (inv_file, num_invigilators, invigilators, Invigilator);
    LOAD (fac_file, num_faculties, faculties, Faculty);
    LOAD_WITH_ID (venue_file, num_venues, next_venue_id, venues, Venue);
    LOAD_WITH_ID (book_file,  num_bookings, next_booking_id, bookings, Booking);
    LOAD_WITH_ID (assign_file,num_assignments, next_assign_id, assignments, Assignment);
}

// Business logic helpers  (caller holds g_lock)
int find_invigilator(const char *u)
{
    for (int i=0;i<num_invigilators;i++)
        if (strcmp(invigilators[i].username,u)==0) return i;
    return -1;
}
int find_faculty(const char *u)
{
    for (int i=0;i<num_faculties;i++)
        if (strcmp(faculties[i].username,u)==0) return i;
    return -1;
}
int find_venue(long long vid)
{
    for (int i=0;i<num_venues;i++)
        if (venues[i].venue_id==vid && venues[i].is_active) return i;
    return -1;
}
int find_booking(long long bid)
{
    for (int i=0;i<num_bookings;i++)
        if (bookings[i].booking_id==bid && bookings[i].is_active) return i;
    return -1;
}

// RACE CONDITION 1 — Venue Double Booking
int venue_is_booked(long long venue_id, const char *date, int sh, int eh)
{
    for (int i=0;i<num_bookings;i++) {
        Booking *b = &bookings[i];
        if (!b->is_active) continue;
        if (b->venue_id != venue_id) continue;
        if (strcmp(b->date, date) != 0) continue;
        if (sh < b->end_hour && eh > b->start_hour)
            return 1;  // overlap 
    }
    return 0;
}

// check if invigilator already has a booking-assignment overlapping this time 
int invigilator_has_clash(const char *username, const char *date, int sh, int eh)
{
    for (int i=0;i<num_assignments;i++) {
        Assignment *a = &assignments[i];
        if (!a->is_active) continue;
        if (a->status == STATUS_CANCELLED) continue;
        if (strcmp(a->invigilator_username, username) != 0) continue;
        if (strcmp(a->date, date) != 0) continue;
        if (sh < a->end_hour && eh > a->start_hour) return 1;
    }
    return 0;
}

// create a PENDING assignment for one invigilator on a booking returns 0=ok, -1=clash, -2=duplicate 
int create_assignment(const char *username, int book_idx)
{
    Booking *b = &bookings[book_idx];
    if (invigilator_has_clash(username, b->date, b->start_hour, b->end_hour))
        return -1;
    for (int i=0;i<num_assignments;i++) {
        Assignment *a = &assignments[i];
        if (a->is_active && a->booking_id==b->booking_id &&
            strcmp(a->invigilator_username, username)==0)
            return -2;
    }
    Assignment a;
    a.assignment_id = next_assign_id++;
    a.booking_id = b->booking_id;
    strcpy(a.invigilator_username, username);
    strcpy(a.venue_name, b->venue_name);
    strcpy(a.date,  b->date);
    a.start_hour = b->start_hour;
    a.end_hour = b->end_hour;
    a.status = STATUS_PENDING;
    a.is_active = 1;
    assignments[num_assignments++] = a;
    return 0;
}

// ADMIN handler
void process_admin(int ns)
{
    // authenticate 
    char password[30];
    read(ns, password, sizeof(password));
    int val = strcmp(password, ADMIN_PASSWORD) == 0 ? 0 : -1;
    write(ns, &val, sizeof(int));
    if (val < 0) return;

    int choice;
    read(ns, &choice, sizeof(int));

    pthread_mutex_lock(&g_lock);

    // 1: View all venues 
    if (choice == 1) {
        int count=0;
        for (int i=0;i<num_venues;i++) if(venues[i].is_active) count++;
        write(ns, &count, sizeof(int));
        for (int i=0;i<num_venues;i++) {
            if (!venues[i].is_active) continue;
            VenueView vv = { venues[i].venue_id, "", venues[i].capacity };
            strcpy(vv.name, venues[i].name);
            write(ns, &vv, sizeof(VenueView));
        }
    }

    // 2: View all bookings 
    else if (choice == 2) {
        int count=0;
        for (int i=0;i<num_bookings;i++) if(bookings[i].is_active) count++;
        write(ns, &count, sizeof(int));
        for (int i=0;i<num_bookings;i++) {
            if (!bookings[i].is_active) continue;
            BookingView bv;
            bv.booking_id = bookings[i].booking_id;
            bv.venue_id = bookings[i].venue_id;
            strcpy(bv.venue_name, bookings[i].venue_name);
            strcpy(bv.faculty_username, bookings[i].faculty_username);
            strcpy(bv.date, bookings[i].date);
            bv.start_hour = bookings[i].start_hour;
            bv.end_hour = bookings[i].end_hour;
            bv.invigilators_needed = bookings[i].invigilators_needed;
            bv.confirmed_count = bookings[i].confirmed_count;
            write(ns, &bv, sizeof(BookingView));
        }
    }

    // 3: Add venue 
    else if (choice == 3) {
        char name[50]; int cap;
        read(ns, name, sizeof(name));
        read(ns, &cap, sizeof(int));
        Venue v;
        v.venue_id = next_venue_id++;
        strcpy(v.name, name);
        v.capacity = cap;
        v.is_active = 1;
        venues[num_venues++] = v;
        printf("[Server] Venue added: ID=%lld Name=%s\n", v.venue_id, v.name);
        write(ns, &v.venue_id, sizeof(long long));
    }

    // 4: Remove venue 
    else if (choice == 4) {
        long long vid; read(ns, &vid, sizeof(long long));
        int idx = find_venue(vid);
        if (idx < 0) { int v=-1; write(ns,&v,sizeof(int)); }
        else {
            venues[idx].is_active = 0;
            for (int i=0;i<num_bookings;i++)
                if (bookings[i].venue_id==vid) bookings[i].is_active=0;
            int v=0; write(ns,&v,sizeof(int));
        }
    }

    // 5: Add invigilator 
    else if (choice == 5) {
        Invigilator inv; read(ns, &inv, sizeof(Invigilator));
        int v = find_invigilator(inv.username)>=0 ? -1 : 0;
        if (v==0) invigilators[num_invigilators++] = inv;
        write(ns, &v, sizeof(int));
    }

    // 6: Remove invigilator 
    else if (choice == 6) {
        char username[30]; read(ns, username, sizeof(username));
        int idx = find_invigilator(username);
        if (idx < 0) { int v=-1; write(ns,&v,sizeof(int)); }
        else {
            for (int i=idx;i<num_invigilators-1;i++) invigilators[i]=invigilators[i+1];
            num_invigilators--;
            for (int i=0;i<num_assignments;i++)
                if (strcmp(assignments[i].invigilator_username,username)==0)
                    assignments[i].is_active=0;
            int v=0; write(ns,&v,sizeof(int));
        }
    }

    // 7: Add faculty 
    else if (choice == 7) {
        Faculty fac; read(ns, &fac, sizeof(Faculty));
        int v = find_faculty(fac.username)>=0 ? -1 : 0;
        if (v==0) faculties[num_faculties++] = fac;
        write(ns, &v, sizeof(int));
    }

    // 8: Remove faculty 
    else if (choice == 8) {
        char username[30]; read(ns, username, sizeof(username));
        int idx = find_faculty(username);
        if (idx < 0) { int v=-1; write(ns,&v,sizeof(int)); }
        else {
            for (int i=idx;i<num_faculties-1;i++) faculties[i]=faculties[i+1];
            num_faculties--;
            for (int i=0;i<num_bookings;i++)
                if (strcmp(bookings[i].faculty_username,username)==0)
                    bookings[i].is_active=0;
            int v=0; write(ns,&v,sizeof(int));
        }
    }

    pthread_mutex_unlock(&g_lock);
}

// FACULTY handler
void process_faculty(int ns)
{
    Faculty fac; read(ns, &fac, sizeof(Faculty));

    pthread_mutex_lock(&g_lock);
    int found = -1;
    for (int i=0;i<num_faculties;i++)
        if (strcmp(faculties[i].username,fac.username)==0 &&
            strcmp(faculties[i].password,fac.password)==0)
            { found=i; break; }
    pthread_mutex_unlock(&g_lock);

    write(ns, &found, sizeof(int));
    if (found < 0) return;

    int choice; read(ns, &choice, sizeof(int));

    pthread_mutex_lock(&g_lock);

    // 1: View available venues for a date + time 
    if (choice == 1) {
        char date[20]; int sh, eh;
        read(ns, date,  sizeof(date));
        read(ns, &sh,   sizeof(int));
        read(ns, &eh,   sizeof(int));
        int count=0;
        for (int i=0;i<num_venues;i++)
            if (venues[i].is_active && !venue_is_booked(venues[i].venue_id,date,sh,eh))
                count++;
        write(ns, &count, sizeof(int));
        for (int i=0;i<num_venues;i++) {
            if (!venues[i].is_active) continue;
            if (venue_is_booked(venues[i].venue_id,date,sh,eh)) continue;
            VenueView vv = { venues[i].venue_id, "", venues[i].capacity };
            strcpy(vv.name, venues[i].name);
            write(ns, &vv, sizeof(VenueView));
        }
    }

    // 2: Book a venue 
    else if (choice == 2) {
        BookingInput inp; read(ns, &inp, sizeof(BookingInput));

        int vidx = find_venue(inp.venue_id);
        if (vidx < 0) { int v=-1; write(ns,&v,sizeof(int)); pthread_mutex_unlock(&g_lock); return; }

        if (venue_is_booked(inp.venue_id, inp.date, inp.start_hour, inp.end_hour)) {
            int v=-2; write(ns,&v,sizeof(int)); pthread_mutex_unlock(&g_lock); return;
        }

        Booking b;
        b.booking_id = next_booking_id++;
        b.venue_id = inp.venue_id;
        strcpy(b.faculty_username, fac.username);
        strcpy(b.venue_name, venues[vidx].name);
        strcpy(b.date, inp.date);
        b.start_hour = inp.start_hour;
        b.end_hour = inp.end_hour;
        b.invigilators_needed = inp.invigilators_needed;
        b.confirmed_count = 0;
        b.is_active = 1;
        bookings[num_bookings++] = b;

        printf("[Server] Venue '%s' booked by '%s' on %s %d:00-%d:00\n",
               b.venue_name, b.faculty_username, b.date, b.start_hour, b.end_hour);

        int v=0; write(ns,&v,sizeof(int));
        write(ns, &b.booking_id, sizeof(long long));
    }

    // 3: View my bookings 
    else if (choice == 3) {
        int count=0;
        for (int i=0;i<num_bookings;i++)
            if (bookings[i].is_active && strcmp(bookings[i].faculty_username,fac.username)==0)
                count++;
        write(ns, &count, sizeof(int));
        for (int i=0;i<num_bookings;i++) {
            Booking *b = &bookings[i];
            if (!b->is_active || strcmp(b->faculty_username,fac.username)!=0) continue;
            BookingView bv;
            bv.booking_id = b->booking_id;
            bv.venue_id = b->venue_id;
            strcpy(bv.venue_name, b->venue_name);
            strcpy(bv.faculty_username, b->faculty_username);
            strcpy(bv.date, b->date);
            bv.start_hour = b->start_hour;
            bv.end_hour = b->end_hour;
            bv.invigilators_needed = b->invigilators_needed;
            bv.confirmed_count = b->confirmed_count;
            write(ns, &bv, sizeof(BookingView));
        }
    }

    // 4: Assign a specific invigilator to my booking 
    else if (choice == 4) {
        char username[30];
        long long booking_id;
        read(ns, username, sizeof(username));
        read(ns, &booking_id, sizeof(long long));

        int bidx = find_booking(booking_id);
        if (bidx < 0) { int v=-1; write(ns,&v,sizeof(int)); pthread_mutex_unlock(&g_lock); return; }

        // ensure this booking belongs to this faculty 
        if (strcmp(bookings[bidx].faculty_username, fac.username)!=0)
            { int v=-2; write(ns,&v,sizeof(int)); pthread_mutex_unlock(&g_lock); return; }

        if (find_invigilator(username) < 0)
            { int v=-3; write(ns,&v,sizeof(int)); pthread_mutex_unlock(&g_lock); return; }

        int r = create_assignment(username, bidx);
        if (r==-1) { int v=-4; write(ns,&v,sizeof(int)); pthread_mutex_unlock(&g_lock); return; }
        if (r==-2) { int v=-5; write(ns,&v,sizeof(int)); pthread_mutex_unlock(&g_lock); return; }

        long long aid = assignments[num_assignments-1].assignment_id;
        write(pipe_fd[1], &aid, sizeof(long long));
        int v=0; write(ns,&v,sizeof(int));
        write(ns, &aid, sizeof(long long));
    }

    // 5: Broadcast request to ALL invigilators for my booking 
    else if (choice == 5) {
        long long booking_id; read(ns, &booking_id, sizeof(long long));

        int bidx = find_booking(booking_id);
        if (bidx < 0) { int v=-1; write(ns,&v,sizeof(int)); pthread_mutex_unlock(&g_lock); return; }

        if (strcmp(bookings[bidx].faculty_username, fac.username)!=0)
            { int v=-2; write(ns,&v,sizeof(int)); pthread_mutex_unlock(&g_lock); return; }

        int sent=0;
        for (int i=0;i<num_invigilators;i++) {
            int r = create_assignment(invigilators[i].username, bidx);
            if (r==0) {
                sent++;
                long long aid = assignments[num_assignments-1].assignment_id;
                write(pipe_fd[1], &aid, sizeof(long long));
                printf("[Server] Request sent to '%s' for booking %lld\n",
                       invigilators[i].username, booking_id);
            }
        }
        write(ns, &sent, sizeof(int));
        int needed = bookings[bidx].invigilators_needed;
        write(ns, &needed, sizeof(int));
    }

    // 6: Change password 
    else if (choice == 6) {
        char new_pass[30]; read(ns, new_pass, sizeof(new_pass));
        strcpy(faculties[found].password, new_pass);
        int v=0; write(ns,&v,sizeof(int));
    }

    pthread_mutex_unlock(&g_lock);
}

// INVIGILATOR handler
void process_invigilator(int ns)
{
    Invigilator inv; read(ns, &inv, sizeof(Invigilator));

    pthread_mutex_lock(&g_lock);
    int found=-1;
    for (int i=0;i<num_invigilators;i++)
        if (strcmp(invigilators[i].username,inv.username)==0 &&
            strcmp(invigilators[i].password,inv.password)==0)
            { found=i; break; }
    pthread_mutex_unlock(&g_lock);

    write(ns, &found, sizeof(int));
    if (found < 0) return;

    int choice; read(ns, &choice, sizeof(int));

    pthread_mutex_lock(&g_lock);

    // 1: View my assignments 
    if (choice == 1) {
        int count=0;
        for (int i=0;i<num_assignments;i++)
            if (assignments[i].is_active &&
                strcmp(assignments[i].invigilator_username,inv.username)==0)
                count++;
        write(ns, &count, sizeof(int));
        for (int i=0;i<num_assignments;i++) {
            Assignment *a = &assignments[i];
            if (!a->is_active || strcmp(a->invigilator_username,inv.username)!=0) continue;
            AssignmentView av;
            av.assignment_id = a->assignment_id;
            av.booking_id = a->booking_id;
            strcpy(av.venue_name, a->venue_name);
            strcpy(av.date, a->date);
            av.start_hour = a->start_hour;
            av.end_hour = a->end_hour;
            av.status = a->status;
            write(ns, &av, sizeof(AssignmentView));
        }
    }

    // 2: Confirm an assignment : RACE CONDITION 2 — Invigilator Confirmation
    else if (choice == 2) {
        long long aid; read(ns, &aid, sizeof(long long));

        int v=-1;
        for (int i=0;i<num_assignments;i++) {
            Assignment *a = &assignments[i];
            if (a->assignment_id != aid) continue;
            if (strcmp(a->invigilator_username,inv.username)!=0) continue;
            if (!a->is_active) break;
            if (a->status==STATUS_CONFIRMED) { v=-2; break; }
            if (a->status==STATUS_CANCELLED) { v=-3; break; }

            int bidx = find_booking(a->booking_id);
            if (bidx < 0) { v=-4; break; }

            Booking *b = &bookings[bidx];
            if (b->confirmed_count >= b->invigilators_needed) {
                a->status = STATUS_CANCELLED;
                v=-5; break;  // slot full, you are cancelled 
            }

            a->status = STATUS_CONFIRMED;
            b->confirmed_count++;
            v=0;

            if (b->confirmed_count == b->invigilators_needed)
                printf("[Server] Booking %lld ('%s') FULLY CONFIRMED! Exam can proceed.\n",
                       b->booking_id, b->venue_name);
            break;
        }
        write(ns, &v, sizeof(int));
    }

    // 3: Change password 
    else if (choice == 3) {
        char new_pass[30]; read(ns, new_pass, sizeof(new_pass));
        strcpy(invigilators[found].password, new_pass);
        int v=0; write(ns,&v,sizeof(int));
    }

    pthread_mutex_unlock(&g_lock);
}

// Thread entry & background threads
struct conn_args { int ns; };

void *process_client(void *arg)
{
    struct conn_args *ca = (struct conn_args *)arg;
    int ns = ca->ns; free(ca);
    int role; read(ns, &role, sizeof(int));
    if (role==1) process_admin(ns);
    else if (role==2) process_faculty(ns);
    else if (role==3) process_invigilator(ns);
    close(ns);
    pthread_exit(0);
}

void *saver_thread(void *arg)
{
    (void)arg;
    while (1) {
        sleep(SAVE_INTERVAL);
        pthread_mutex_lock(&g_lock);
        save_all();
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

void *pipe_reader_thread(void *arg)
{
    (void)arg;
    long long aid;
    while (1)
        if (read(pipe_fd[0], &aid, sizeof(long long)) > 0)
            printf("[IPC] Assignment request created — ID: %lld\n", aid);
    return NULL;
}

// main
int main()
{
    if (pipe(pipe_fd) < 0) { perror("pipe"); return 1; }
    pthread_mutex_init(&g_lock, NULL);

    pthread_mutex_lock(&g_lock);
    load_all();
    pthread_mutex_unlock(&g_lock);
    printf("[Server] Data loaded. Invigilators=%d Faculty=%d Venues=%d Bookings=%d\n",
           num_invigilators, num_faculties, num_venues, num_bookings);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, saver_thread, NULL); pthread_detach(t1);
    pthread_create(&t2, NULL, pipe_reader_thread, NULL); pthread_detach(t2);

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    int opt=1; setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server, client;
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(PORT);
    bind(sd, (struct sockaddr*)&server, sizeof(server));
    listen(sd, 10);
    printf("[Server] Listening on port %d ...\n", PORT);

    pthread_t tid;
    while (1) {
        socklen_t len = sizeof(client);
        struct conn_args *ca = malloc(sizeof(struct conn_args));
        ca->ns = accept(sd, (struct sockaddr*)&client, &len);
        pthread_create(&tid, NULL, process_client, ca);
        pthread_detach(tid);
    }
}