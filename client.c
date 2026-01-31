#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <ctype.h>
#include <sys/stat.h>


#define BUFFER_SIZE 512
#define EXIT_KEYWORD "EXIT"
#define SHOW_GROUPS "SHOW_ALL_GROUPS"
#define SHOW_CLIENTS "SHOW_ALL_CLIENTS"
#define KICKED_OUT_MESSAGE ">> Kicked Out..."
#define REPORT_KICKED_OUT_MESSAGE ">> Kicked Out because of multiple reports..."
#define USERNAME_MESSAGE ">> USERNAME HAS ALREADY BEEN TAKEN..."

void error(const char* msg) {
    perror(msg);
    exit(0);
}

// unique sockedId for a client
int socket_fd;

// message receiver from other clients will be present inside this file
char* fileName;

void sanitize_filename(char *name) {
    for (int i = 0; name[i]; i++) {
        if (!(isalnum(name[i]) || name[i] == '.' || name[i] == '_')) {
            name[i] = '_';
        }
    }
}

bool is_safe_filename(const char *name) {
    if (strstr(name, "..")) return false;
    if (strchr(name, '/')) return false;
    if (strchr(name, '\\')) return false;
    return true;
}


void encrypt_message(char* input, char* encrypted) {
    // to send encrypted message to server
    // message format "@username <message>"
    // file format "@username @file <file name>"
    // report format "#username"
    // group format "$CREATE <groupName>" , "$JOIN <groupName>" "$LEAVE <groupName>" no encryption needed , as it is request to server
    // group format "$groupName <message>" 
    // only message part needs to be encrypted

    int len = strlen(input);
    int pos = 0;
    int last = 0;
    if(!strcmp(input, EXIT_KEYWORD) || !strcmp(input, SHOW_CLIENTS) || !strcmp(input, SHOW_GROUPS)) {
        while(input[pos] != '\0') encrypted[last++] = input[pos++];
        encrypted[last]='\0';
        return ;
    }

    // for group
    if(input[0] == '$'){
        char query[256], message[256];
        sscanf(input,"$%s %s",query,message);
        // no encryption for request to server
        if(!strcmp(query,"CREATE") || !strcmp(query,"JOIN") || !strcmp(query,"LEAVE") ) {
            while(input[pos] != '\0') encrypted[last++] = input[pos++];
            encrypted[last]='\0';
            return ;
        }
        // add "$groupName" to encryted messagage
        else{
            while(input[pos] != ' ') encrypted[last++] = input[pos++];
            encrypted[last++]=input[pos++];
        }
    }

    // add "@username" or "#username" to encryted message
    if(input[0] == '@' || input[0] == '#') {
        while(input[last] != ' ' && input[last] != '\0') { 
            encrypted[pos++] = input[last++];
        }
        if(input[last] == ' ') encrypted[pos++] = input[last++];
    }

    // for adding the file text to encrypt message
    if(input[last] == '@') {
        while(input[last] != ' ' && input[last] != '\0') { 
            encrypted[pos++] = input[last++];
        }
        if(input[last] == ' ') encrypted[pos++] = input[last++];

        // reads the fileName from the input message 
        char* fileName = (char*)malloc(sizeof(char) * (len-last+1));
        snprintf(fileName, len - last + 1, "%s", input + last);

        while(input[last] != '\0') {
            encrypted[pos++] = input[last++];
        }
        encrypted[pos++] = ' ';
        
        char* temp = (char*)realloc(input, sizeof(char) * 1024);
        if(temp == NULL) {
            fprintf(stderr, "Memory reallocation failed\n");
            return;
        }
        input = temp;
        input[last++] = ' ';

        // open the file "fileName" and add the file content to the encrpt message which will be encrypted
        FILE* file = fopen(fileName, "r");
        char fileData[1024];
        fgets(fileData, sizeof(fileData), file);
        
        fclose(file);
        strcat(input, fileData);
    }
    
    // encrypt the message or fileContent
    // takes the last character and add 3 digit ascii code in the encrypt message
    len = strlen(input);
    for(int i = len - 1; i >= last; i--) {
        pos += sprintf(encrypted + pos, "%03d", (unsigned char)input[i]);
    }

    encrypted[pos] = '\0'; 
}

void decrypt_message(char* input, char* decrypted) {
    // to dencrypt message received through server
    // message format "@username[H:M] <message>"
    // file format "@username[H:M] @file <file name> <content>"
    // only message part needs to be dencrypted
    
    int i = 0, j = 0;
    char fileName[256];
    bool gotFile = false;
    memset(decrypted, 0, BUFFER_SIZE * 3);

    // no decryption for direct response from server
    if(input[0] == '>'){
        while(input[i] != '.') decrypted[j++] = input[i++];
        decrypted[j]='\0';
        return ;
    }

    // copy the input message till username[H:M]
    while(input[i] != ' ') decrypted[j++] = input[i++]; 
    decrypted[j++] = input[i++];
    decrypted[j++] = input[i++];
    decrypted[j++] = input[i++];

    // if input message type is file, we extract file name 
    if(input[i] == '@') {
        gotFile = true;
        i += 6;
        int k = 1;
        while(input[i + k - 1] != ' ') k++;
        strncpy(fileName, input + i, k-1);
        fileName[k-1] = '\0';
        i += k;
    }

    // decrypt the message or file content
    int len = strlen(input);
    char temp[4]; 
    int revIndex = 0, revLen = (len - i) / 3;
    char reversed[revLen + 1];

    while(input[i] >= '0' && input[i] <= '9' && input[i+1] >= '0' && input[i+1] <= '9' && input[i+2] >= '0' && input[i+2] <= '9') {
        strncpy(temp, input + i, 3);
        temp[3] = '\0';
        reversed[revIndex++] = (char)atoi(temp);
        i += 3;
    }
    reversed[revIndex] = '\0';

    if(!gotFile) {
        // if input was message type, add the message to decrypted message
        for(int k = revIndex - 1; k >= 0; k--) decrypted[j++] = reversed[k];
        decrypted[j] = '\0';
    }
    else {
        // if input was file type, open the file <file name> and add the content
        // inform the receiver that a file was sent by @username
        // Validate filename
        if (!is_safe_filename(fileName)) {
            strcat(decrypted, "Received a file with unsafe filename. File rejected.");
            return;
       }
        sanitize_filename(fileName);

        // Build final filename
        char final_path[512];
        time_t now = time(NULL);
        char message[512];

        snprintf(final_path, sizeof(final_path),
                "received_files/%s_%ld_%s",
                decrypted, now, fileName);

        // Replace spaces from username prefix if any
        for (int i = 0; final_path[i]; i++) {
            if (final_path[i] == ' ') final_path[i] = '_';
        }

        // Write file safely
        FILE* file = fopen(final_path, "w");
        if (!file) {
            perror("File creation failed");
            return;
        }

        for (int k = revIndex - 1; k >= 0; k--) {
            fputc(reversed[k], file);
        }
        fclose(file);
        // Inform user
        char msg[512];
        snprintf(msg, sizeof(msg),
                "Received file saved as %s", final_path);
        strcat(decrypted, msg);

        memset(message, 0, sizeof(message));
        snprintf(message, sizeof(message), "I sent you a file named %s\n", fileName);
        strcat(decrypted, message);
    }
}

void* listen_for_messages(void* arg) {
    char* buffer = (char*)malloc(BUFFER_SIZE);
    char* decrypt = (char*)malloc(BUFFER_SIZE * 3);
    
    // opening the Chat file
    FILE* chatPad = (FILE*)arg;
    if(!chatPad) {
        perror("Error opening chat file in thread");
        return NULL;
    }

    while(true) {
        memset(buffer, 0, BUFFER_SIZE);
        memset(decrypt, 0, BUFFER_SIZE * 3);
        ssize_t n = read(socket_fd, buffer, BUFFER_SIZE - 1);
        if(n > 0) {
            buffer[n] = '\0';
            // if message received from server contains "Kicked Out"
            if(strcmp(buffer, KICKED_OUT_MESSAGE) == 0 || 
               strcmp(buffer, REPORT_KICKED_OUT_MESSAGE) == 0 || 
               strcmp(buffer, ">> Kicked Out due to idleness...") == 0){
                printf("\nYou have been kicked out...\n");
                fclose(chatPad);
                _exit(EXIT_FAILURE);
            }
            if(strcmp(buffer, USERNAME_MESSAGE) == 0){
                printf("\n%s\n", USERNAME_MESSAGE);
                fclose(chatPad);
                _exit(EXIT_FAILURE);
            }

            // decrypt the message received through server
            decrypt_message(buffer, decrypt);
            fprintf(chatPad, "%s\n", decrypt);
            fflush(chatPad); 
            fflush(stdout);
        } 
        else if(n == 0) {
            // server has closed the connection
            printf("\nServer closed the connection. Exiting...\n");
            fclose(chatPad);
            exit(0);
        } 
        else error("ERROR reading from socket");
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    int port_no;
    struct sockaddr_in server_address;
    struct hostent* server;
    mkdir("received_files", 0755);
    mkdir("chat_logs", 0755);
    char* buffer = (char*)malloc(BUFFER_SIZE);
    char* encrypt = (char*)malloc(BUFFER_SIZE * 3);

    if(argc < 4) {
        fprintf(stderr, "Usage: %s <hostname> <port> <name>\n", argv[0]);
        exit(1);
    }

    // port number is thrid element 
    port_no = atoi(argv[2]);

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(socket_fd < 0) error("ERROR opening socket");

    // fetching host name using the ip address
    server = gethostbyname(argv[1]);
    if(server == NULL) {
        fprintf(stderr, "ERROR no such host\n");
        exit(0);
    }

    // clearing server_address and creating the headers and payload for connection
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    memcpy(&server_address.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    server_address.sin_port = htons(port_no);

    // connecting to server
    if(connect(socket_fd, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) error("ERROR connecting");

    // assigning the name to the client
    bzero(buffer, BUFFER_SIZE);
    snprintf(buffer, BUFFER_SIZE, "%s", argv[3]);

    //opening the file with client name to see the message from other clients
    fileName = (char*)malloc(strlen(argv[3]) + 5);
    if(!fileName) error("Memory allocation failed");
    snprintf(fileName, strlen(argv[3]) + 20, "chat_logs/%s.log", argv[3]);
    printf("fileName: %s\n", fileName);

    FILE* chatPad = fopen(fileName, "w");
    if(!chatPad) error("ERROR opening chat file");

    // sending name of client to server
    if(write(socket_fd, buffer, strlen(buffer)) < 0) error("ERROR sending name");

    // creating the thread for receiving response through server
    pthread_t listener_thread;
    if(pthread_create(&listener_thread, NULL, listen_for_messages, chatPad) != 0) error("ERROR creating thread");

    while(true) {
        printf("Enter message: ");
        memset(buffer, 0, BUFFER_SIZE);
        fgets(buffer, BUFFER_SIZE, stdin);
        buffer[strcspn(buffer, "\n")] = 0; 

        fprintf(chatPad, "Me: %s\n", buffer);
        fflush(chatPad); 

        // encrypting the message of client and then sending the encrypted message to server
        memset(encrypt, 0, sizeof(encrypt));
        encrypt_message(buffer, encrypt);
        if(write(socket_fd, encrypt, strlen(encrypt)) < 0) error("ERROR writing to socket");
    }

    fclose(chatPad);
    free(fileName);
    close(socket_fd);
    return 0;
}