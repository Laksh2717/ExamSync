#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>

#define PORT 8080
#define SERVER_IP "127.0.0.1"

// assignment status 
#define STATUS_PENDING 0
#define STATUS_CONFIRMED 1
#define STATUS_CANCELLED 2

// Packed structs — must match server exactly
#pragma pack(1)
typedef struct { char username[30]; char password[30]; } Invigilator;
typedef struct { char username[30]; char password[30]; } Faculty;

typedef struct {
    long long venue_id;
    char name[50];
    int capacity;
} VenueView;

typedef struct {
    long long venue_id;
    char date[20];
    int start_hour;
    int end_hour;
    int invigilators_needed;
} BookingInput;

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

// Display helpers
void print_venue(VenueView *v)
{
    printf("Venue ID : %lld\n", v->venue_id);
    printf("Name : %s\n",   v->name);
    printf("Capacity : %d\n\n", v->capacity);
}

void print_booking(BookingView *b)
{
    printf("Booking ID : %lld\n", b->booking_id);
    printf("Venue : %s (ID: %lld)\n", b->venue_name, b->venue_id);
    printf("Booked by : %s\n",   b->faculty_username);
    printf("Date : %s\n",   b->date);
    printf("Time : %d:00 to %d:00\n", b->start_hour, b->end_hour);
    printf("Invigilators Needed : %d\n",   b->invigilators_needed);
    printf("Confirmed So Far : %d\n\n", b->confirmed_count);
}

void print_assignment(AssignmentView *a)
{
    const char *s = a->status==STATUS_PENDING ? "Pending" : a->status==STATUS_CONFIRMED ? "Confirmed" : "Cancelled";
    printf("Assignment  : %lld\n", a->assignment_id);
    printf("Booking ID : %lld\n", a->booking_id);
    printf("Venue : %s\n",   a->venue_name);
    printf("Date : %s\n",   a->date);
    printf("Time : %d:00 to %d:00\n", a->start_hour, a->end_hour);
    printf("Status : %s\n\n", s);
}

// ADMIN menu
void handle_admin(int sd)
{
    char password[30];
    printf("Enter admin password: ");
    scanf("%s", password);
    write(sd, password, sizeof(password));

    int val; read(sd, &val, sizeof(int));
    if (val < 0) { printf("Wrong password!\n"); return; }

    printf("\nAdmin Menu:\n");
    printf("1. View all venues\n");
    printf("2. View all bookings\n");
    printf("3. Add venue\n");
    printf("4. Remove venue\n");
    printf("5. Add invigilator\n");
    printf("6. Remove invigilator\n");
    printf("7. Add faculty\n");
    printf("8. Remove faculty\n");
    printf("Enter choice: ");

    int choice; scanf("%d", &choice);
    write(sd, &choice, sizeof(int));

    if (choice == 1) {
        int count; read(sd, &count, sizeof(int));
        if (count==0) { printf("No venues found.\n"); return; }
        printf("\n--- Venues (%d) ---\n", count);
        for (int i=0;i<count;i++) {
            VenueView vv; read(sd, &vv, sizeof(VenueView)); print_venue(&vv);
        }
    }
    else if (choice == 2) {
        int count; read(sd, &count, sizeof(int));
        if (count==0) { printf("No bookings found.\n"); return; }
        printf("\n--- All Bookings (%d) ---\n", count);
        for (int i=0;i<count;i++) {
            BookingView bv; read(sd, &bv, sizeof(BookingView)); print_booking(&bv);
        }
    }
    else if (choice == 3) {
        char name[50]; int cap;
        printf("Enter venue name : "); scanf(" %[^\n]", name);
        printf("Enter venue capacity: "); scanf("%d", &cap);
        write(sd, name, sizeof(name));
        write(sd, &cap, sizeof(int));
        long long vid; read(sd, &vid, sizeof(long long));
        printf("Venue added! Venue ID: %lld\n", vid);
    }
    else if (choice == 4) {
        long long vid; printf("Enter Venue ID to remove: "); scanf("%lld", &vid);
        write(sd, &vid, sizeof(long long));
        int v; read(sd, &v, sizeof(int));
        printf(v<0 ? "Venue not found!\n" : "Venue removed successfully!\n");
    }
    else if (choice == 5) {
        Invigilator inv;
        printf("Enter username: "); scanf("%s", inv.username);
        printf("Enter password: "); scanf("%s", inv.password);
        write(sd, &inv, sizeof(Invigilator));
        int v; read(sd, &v, sizeof(int));
        printf(v<0 ? "Username already exists!\n" : "Invigilator added successfully!\n");
    }
    else if (choice == 6) {
        char username[30]; printf("Enter username to remove: "); scanf("%s", username);
        write(sd, username, sizeof(username));
        int v; read(sd, &v, sizeof(int));
        printf(v<0 ? "Invigilator not found!\n" : "Invigilator removed successfully!\n");
    }
    else if (choice == 7) {
        Faculty fac;
        printf("Enter username: "); scanf("%s", fac.username);
        printf("Enter password: "); scanf("%s", fac.password);
        write(sd, &fac, sizeof(Faculty));
        int v; read(sd, &v, sizeof(int));
        printf(v<0 ? "Username already exists!\n" : "Faculty added successfully!\n");
    }
    else if (choice == 8) {
        char username[30]; printf("Enter username to remove: "); scanf("%s", username);
        write(sd, username, sizeof(username));
        int v; read(sd, &v, sizeof(int));
        printf(v<0 ? "Faculty not found!\n" : "Faculty removed successfully!\n");
    }
    else printf("Invalid choice.\n");
}

// FACULTY menu
void handle_faculty(int sd)
{
    Faculty fac;
    printf("Enter username: "); scanf("%s", fac.username);
    printf("Enter password: "); scanf("%s", fac.password);
    write(sd, &fac, sizeof(Faculty));

    int found; read(sd, &found, sizeof(int));
    if (found < 0) { printf("Invalid credentials!\n"); return; }

    printf("\nFaculty Menu:\n");
    printf("1. View available venues\n");
    printf("2. Book a venue\n");
    printf("3. View my bookings\n");
    printf("4. Assign specific invigilator to my booking\n");
    printf("5. Broadcast request to all invigilators\n");
    printf("6. Change password\n");
    printf("Enter choice: ");

    int choice; scanf("%d", &choice);
    write(sd, &choice, sizeof(int));

    if (choice == 1) {
        char date[20]; int sh, eh;
        printf("Enter date (DD-MM-YYYY): "); scanf("%s", date);
        printf("Enter start hour (0-23): "); scanf("%d", &sh);
        printf("Enter end   hour (0-23): "); scanf("%d", &eh);
        write(sd, date, sizeof(date));
        write(sd, &sh,  sizeof(int));
        write(sd, &eh,  sizeof(int));
        int count; read(sd, &count, sizeof(int));
        if (count==0) { printf("No venues available for this date and time.\n"); return; }
        printf("\n--- Available Venues (%d) ---\n", count);
        for (int i=0;i<count;i++) {
            VenueView vv; read(sd, &vv, sizeof(VenueView)); print_venue(&vv);
        }
    }
    else if (choice == 2) {
        BookingInput inp;
        printf("Enter Venue ID : "); scanf("%lld", &inp.venue_id);
        printf("Enter date (DD-MM-YYYY) : "); scanf("%s", inp.date);
        printf("Enter start hour (0-23) : "); scanf("%d", &inp.start_hour);
        printf("Enter end   hour (0-23) : "); scanf("%d", &inp.end_hour);
        printf("Invigilators needed : "); scanf("%d", &inp.invigilators_needed);
        write(sd, &inp, sizeof(BookingInput));
        int v; read(sd, &v, sizeof(int));
        if (v==0) {
            long long bid; read(sd, &bid, sizeof(long long));
            printf("Venue booked successfully! Booking ID: %lld\n", bid);
        }
        else if (v==-1) printf("Venue not found!\n");
        else if (v==-2) printf("Venue already booked for this date and time!\n");
    }
    else if (choice == 3) {
        int count; read(sd, &count, sizeof(int));
        if (count==0) { printf("No bookings found.\n"); return; }
        printf("\n--- My Bookings (%d) ---\n", count);
        for (int i=0;i<count;i++) {
            BookingView bv; read(sd, &bv, sizeof(BookingView)); print_booking(&bv);
        }
    }
    else if (choice == 4) {
        char username[30]; long long booking_id;
        printf("Enter invigilator username: "); scanf("%s", username);
        printf("Enter Booking ID: "); scanf("%lld", &booking_id);
        write(sd, username,    sizeof(username));
        write(sd, &booking_id, sizeof(long long));
        int v; read(sd, &v, sizeof(int));
        if (v==0) {
            long long aid; read(sd, &aid, sizeof(long long));
            printf("Assignment created! Assignment ID: %lld\n", aid);
        }
        else if (v==-1) printf("Booking not found!\n");
        else if (v==-2) printf("This booking does not belong to you!\n");
        else if (v==-3) printf("Invigilator not found!\n");
        else if (v==-4) printf("Invigilator has a time clash!\n");
        else if (v==-5) printf("Invigilator already assigned to this booking!\n");
    }
    else if (choice == 5) {
        long long booking_id;
        printf("Enter Booking ID: "); scanf("%lld", &booking_id);
        write(sd, &booking_id, sizeof(long long));
        int sent, needed;
        read(sd, &sent,   sizeof(int));
        read(sd, &needed, sizeof(int));
        if (sent==-1) printf("Booking not found!\n");
        else if (sent==-2) printf("This booking does not belong to you!\n");
        else printf("Broadcast sent to %d invigilators!\n" "First %d to confirm will be allocated.\n", sent, needed);
    }
    else if (choice == 6) {
        char new_pass[30]; printf("Enter new password: "); scanf("%s", new_pass);
        write(sd, new_pass, sizeof(new_pass));
        int v; read(sd, &v, sizeof(int));
        printf(v<0 ? "Error changing password!\n" : "Password changed successfully!\n");
    }
    else printf("Invalid choice.\n");
}

// INVIGILATOR menu
void handle_invigilator(int sd)
{
    Invigilator inv;
    printf("Enter username: "); scanf("%s", inv.username);
    printf("Enter password: "); scanf("%s", inv.password);
    write(sd, &inv, sizeof(Invigilator));

    int found; read(sd, &found, sizeof(int));
    if (found < 0) { printf("Invalid credentials!\n"); return; }

    printf("\nInvigilator Menu:\n");
    printf("1. View my assignments\n");
    printf("2. Confirm an assignment\n");
    printf("3. Change password\n");
    printf("Enter choice: ");

    int choice; scanf("%d", &choice);
    write(sd, &choice, sizeof(int));

    if (choice == 1) {
        int count; read(sd, &count, sizeof(int));
        if (count==0) { printf("No assignments found.\n"); return; }
        printf("\n--- My Assignments (%d) ---\n", count);
        for (int i=0;i<count;i++) {
            AssignmentView av; read(sd, &av, sizeof(AssignmentView)); print_assignment(&av);
        }
    }
    else if (choice == 2) {
        long long aid; printf("Enter Assignment ID to confirm: "); scanf("%lld", &aid);
        write(sd, &aid, sizeof(long long));
        int v; read(sd, &v, sizeof(int));
        if (v== 0) printf("Assignment confirmed successfully!\n");
        else if (v==-2) printf("Already confirmed!\n");
        else if (v==-3) printf("Already cancelled!\n");
        else if (v==-5) printf("Sorry, this slot is already fully staffed. Your request has been cancelled.\n");
        else printf("Assignment not found!\n");
    }
    else if (choice == 3) {
        char new_pass[30]; printf("Enter new password: "); scanf("%s", new_pass);
        write(sd, new_pass, sizeof(new_pass));
        int v; read(sd, &v, sizeof(int));
        printf(v<0 ? "Error changing password!\n" : "Password changed successfully!\n");
    }
    else printf("Invalid choice.\n");
}

// main
int main()
{
    int sd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port   = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server.sin_addr);

    if (connect(sd, (struct sockaddr*)&server, sizeof(server)) < 0) {
        printf("Connection to server failed!\n");
        return 1;
    }

    printf("=== Campus Exam Invigilator Scheduling System ===\n");
    printf("1. Admin\n2. Faculty\n3. Invigilator\n");
    printf("Enter choice: ");

    int choice; scanf("%d", &choice);
    write(sd, &choice, sizeof(int));

    if (choice==1) handle_admin(sd);
    else if (choice==2) handle_faculty(sd);
    else if (choice==3) handle_invigilator(sd);
    else printf("Invalid choice.\n");

    close(sd);
    return 0;
}