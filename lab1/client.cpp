#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
// g++ client.cpp -o client

int main() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in serverAddr;
    serverAddr.sin_port = htons(8080);
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    while(true){
        std::string userunput;
        std::cout << "Введите строку: ";
        std::getline(std::cin, userunput);
        const char* mes = userunput.c_str();
        sendto(sockfd, mes, sizeof(mes), 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
        char buffer [1024];
        recvfrom(sockfd, buffer, 1024, 0, NULL, NULL);
        std::cout << "Server: " << buffer << std::endl;
        if(userunput == "0"){
            break;
        }
    }
    close(sockfd);
    return 0;
}