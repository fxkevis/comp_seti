#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
// g++ server.cpp -o cerver

int main() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in serverAddr, clientAddr;
    serverAddr.sin_port = htons(8080);
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    bind(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    char buffer [1024];

    socklen_t addrLen = sizeof(clientAddr);
    while(true){
        int n = recvfrom(sockfd, buffer, 1024, 0, (struct sockaddr*)&clientAddr, &addrLen);
        buffer[n] = '\0';
        std::cout << "Client:" << buffer << std::endl;
        sendto(sockfd, "Hello, from server!", 20, 0, (struct sockaddr*)&clientAddr, addrLen);
        if(buffer[0] == '0'){
            break;
            sendto(sockfd, "0", 20, 0, (struct sockaddr*)&clientAddr, addrLen);
        }
    }
    close(sockfd);
    return 0;
}