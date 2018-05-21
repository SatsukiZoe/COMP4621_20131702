#include <iostream>
#include <cstdio>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <chrono>
#include <ctime>
#include <arpa/inet.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <zlib.h>
#include <map>

const bool DEBUG = false;

// const int MAXLINE = 100;
const int SERVER_PORT = 12345;
const int LISTENNQ = 5;
const int BUFFERSIZE = 100;

int listenfd;

std::map<std::string, std::string> contentTypes;

std::string currentTime() {
    auto currentTime = std::chrono::system_clock::to_time_t( std::chrono::system_clock::now() );
    auto t = gmtime(&currentTime);

    std::string weekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    std::string months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::stringstream ss;
    ss << weekdays[t->tm_wday] << ", ";
    ss << (t->tm_mday < 10 ? "0" : "") << t->tm_mday;
    ss << " " << months[t->tm_mon] << " " << (t->tm_year + 1900) << " ";
    ss << (t->tm_hour < 10 ? "0" : "") << t->tm_hour << ":";
    ss << (t->tm_min < 10 ? "0" : "") << t->tm_min << ":";
    ss << (t->tm_sec < 10 ? "0" : "") << t->tm_sec << " GMT";
    return ss.str();
}

// compress data with gzip
std::string compress(const std::string& input, int compressionlevel = Z_BEST_COMPRESSION) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    if (deflateInit2(&zs, compressionlevel, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw(std::runtime_error("deflateInit failed while compressing."));
    }

    // set the z_stream's input
    zs.next_in = (Bytef*) input.data();
    zs.avail_in = (uint) input.size();


    int ret;
    char outbuffer[32768];
    std::string outstring;

    // retrieve the compressed bytes blockwise
    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        ret = deflate(&zs, Z_FINISH);

        if (outstring.size() < zs.total_out) {
            // append the block to the output string
            outstring.append(outbuffer, zs.total_out - outstring.size());
        }
    } while (ret == Z_OK);

    deflateEnd(&zs);

    if (ret != Z_STREAM_END) {
        std::ostringstream oss;
        oss << "Exception during zlib compression: (" << ret << ") " << zs.msg;
        throw(std::runtime_error(oss.str()));
    }

    return outstring;
}

//receive requests
std::string recvice(int connection) {
    int bytesRecived = 0;
    char reciveBuffer[BUFFERSIZE] = {0};
    std::string result = "";
    do {
        bytesRecived = recv(connection, reciveBuffer, BUFFERSIZE - 1, 0);
        reciveBuffer[bytesRecived] = '\0'; //end of cstd::string
        result += reciveBuffer;
    } while(bytesRecived >= 99);

    return result;
}

void sendResponse(int connection, std::string response) {
    write(connection, response.c_str(), response.length());
    return;
}

void handleRequest(int connection) {
    // TODO: analyze request
    std::string requestMessage = recvice(connection);
    std::string messageHeader = "";
    std::string messageBody = "";

    // debug
    if(DEBUG) {
        std::cout << requestMessage << std::endl;
    }

    // seperate message header and message body
    size_t endOfMessageHeader = requestMessage.find("\r\n\r\n");
    if(endOfMessageHeader != std::string::npos){ //can find CRLF -> have body
        messageHeader = requestMessage.substr(0,endOfMessageHeader);
        messageBody = requestMessage.substr(endOfMessageHeader + 4);
    } else {
        messageHeader = requestMessage;
    }

    // debug
    if(DEBUG) {
        std::cout << "==== header ====" << std::endl << messageHeader << std::endl << "==== end of header ====" << std::endl;
        std::cout << "==== body ====" << std::endl << messageBody << std::endl << "==== end of body ====" << std::endl;
    }

    // Analyze request line from request message header
    std::string requestLine = "";
    std::string requestHeaders = "";
    size_t endOfrequestLine = messageHeader.find("\r\n");
    if(endOfrequestLine != std::string::npos){
        requestLine = messageHeader.substr(0, endOfrequestLine);
        requestHeaders = messageHeader.substr(endOfrequestLine + 2);
    } else {
        requestLine = messageHeader;
    }
    //=====analyzing the status Line=====
    // debug
    if(DEBUG) {
        std::cout << "Status line" << std::endl;
        std::cout << requestLine << std::endl;
    }

    //check GET method
    std::string method = "";
    std::string pathway = "";
    size_t checkStatus = requestLine.find(" ");
    if(checkStatus != std::string::npos){
        method = messageHeader.substr(0,checkStatus);
        pathway = messageHeader.substr(checkStatus+1);
    } else {
        std::cout << "Line 109: Request-Line not correct: " << requestLine << std::endl;
        // close the connection
        close(connection);
        return;
    }

    // debug
    if(DEBUG) {
        std::cout << "Method: " << method << std::endl;
    }

    // only accept GET method
    if(method != "GET") {
        // close the connection
        close(connection);
        return;
    }
    //=========extracting URI=========
    std::string URL = "";
    std::string HTTPVersion = "";
    size_t path = pathway.find(" ");
    if(path != std::string::npos){
        URL = pathway.substr(0, path);
        HTTPVersion = pathway.substr(path + 1);
    } else {
        std::cout << "Request-Line not correct: " << requestLine << std::endl;
        // close the connection
        close(connection);
        return;
    }

    // Finished request line

    bool needGzip = false;
    bool needCloseConnection = false;
    // Analyze request headers
    do {
        size_t index = requestHeaders.find("\r\n");

        std::string requestHeader = "";
        if(index != std::string::npos) {
            requestHeader = requestHeaders.substr(0, index);
            requestHeaders = requestHeaders.substr(index + 2);
        } else {
            requestHeader = requestHeaders;
            requestHeaders = "";
        }

        std::string requestHeaderField = "";
        std::string requestHeaderParam = "";
        index = requestHeader.find(":");

        if(index != std::string::npos) {
            requestHeaderField = requestHeader.substr(0, index);
            requestHeaderParam = requestHeader.substr(index + 1);
        } else {
            continue;
        }

        if(requestHeaderField == "Accept-Encoding"){
            size_t pos = requestHeaderParam.find("gzip");
            if(pos != std::string::npos) {
                needGzip = true;
            }
        }

        if(requestHeaderField == "Connection"){
            size_t pos = requestHeaderParam.find("close");
            if(pos != std::string::npos) {
                needCloseConnection = true;
            }
        }

    } while(requestHeaders != "");

    //Prepare the file
    bool validPath = false;
    std::string filePath = URL;

    //check valid pathway
    while(!validPath) {
        if(filePath.substr(0,3)=="../"){
            filePath = filePath.substr(3);
        }
        else{
            validPath = true;
        }
    }

    //broswer default behaviour
    if(filePath.length() == 1 && filePath[0] == '/'){
        filePath += "index.html";
    }

    //convert to relative path to the doc root of the server
    if(filePath[0] == '/') {
        filePath = "." + filePath;
    }

    // check for file extension
    std::string fileExtension = "";

    size_t extension = filePath.rfind(".");
    if(extension != std::string::npos){
        fileExtension = filePath.substr(extension + 1);
    }

    std::string contentType = "";
    bool binaryFile = false;
    //TODO : change to if
    auto contentTypeIterator = contentTypes.find(fileExtension);
    if(contentTypeIterator != contentTypes.end()) {
        contentType = contentTypeIterator->second;
    } else {
        contentType = "text/plain";
    }

    if(contentType.substr(0, 4) != "text") {
        binaryFile = true;
    }

    //check the existence of the file(s)
    auto flags = std::ifstream::in;
    if(binaryFile) {
        flags |= std::ifstream::binary;
    }

    std::ifstream f(filePath.c_str(), flags);

    //if file not exist
    if(!f.good()){
        std::string response = "HTTP/1.1 404 Not found\r\n";
        response += "Date: " + currentTime() + "\r\n";
        response += "Server: Zoe Server (0.0.1)\r\n";
        response += "Content-Type: text/html; charset=utf-8\r\n";
        response += "Content-Length: 23\r\n";
        response += "\r\n";
        // msg body starts
        response += "<h1>File not found</h1>";
        // send response
        sendResponse(connection, response);

        // close the connection
        close(connection);

        return;
    }

    std::stringstream content;
    content << f.rdbuf();

    f.close();

    std::string contentString = content.str();

    // Compress the file content if gzip is enabled
    if(needGzip) {
        std::cout << "compressing " << filePath << "(" << contentString.length() << ")" << std::endl;
        contentString = compress(contentString);
        std::cout << "compressed size: " << contentString.length() << std::endl;
    }

    //-------return content-------

    // TODO: construct response header
    std::string response = "HTTP/1.1 200 OK\r\n";
    response += "Date: " + currentTime() + "\r\n";
    response += "Server: Zoe Server (0.0.1)\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    if(needGzip) {
        response += "Content-Encoding: gzip\r\n";
    }
    response += "Transfer-Encoding: chunked\r\n";
    response += "\r\n";

    // send response header
    sendResponse(connection, response);

    if(DEBUG) {
        std::cout << "start chunking" << std::endl;
    }

    int chunkSize = 1024 * 1024;
    int chunkID = 0;
    do {
        chunkID++;

        if(contentString.length() > chunkSize) {
            std::string chunk = contentString.substr(0, chunkSize);
            contentString = contentString.substr(chunkSize);

            if(DEBUG) {
                std::cout << "chunk #" << chunkID << " length: " << chunk.length() << std::endl;
            }

            std::stringstream chunkSizeString;
            chunkSizeString << std::hex << chunk.length();
            sendResponse(connection, chunkSizeString.str() + "\r\n" + chunk + "\r\n");
        } else {
            std::stringstream chunkSizeString;
            chunkSizeString << std::hex << contentString.length();

            sendResponse(connection, chunkSizeString.str() + "\r\n" + contentString + "\r\n0\r\n\r\n");

            std::cout << "done transfer" << std::endl;
            break;
        }
    } while(true);

    // close the connection
    if(needCloseConnection) {
        close(connection);
        return;
    } else {
        handleRequest(connection);
    }
}

void initializeContentTypes() {
    contentTypes["html"] = "text/html";
    contentTypes["htm"] = "text/html";
    contentTypes["css"] = "text/css";
    contentTypes["js"] = "text/javascript";
    contentTypes["jpg"] = "image/jpeg";
    contentTypes["jpeg"] = "image/jpeg";
    contentTypes["pdf"] = "application/pdf";
    contentTypes["pptx"] = "application/vnd.openxmlformats-officedocument.presentationml.presentation";
}

int main(int argc, char **argv) {

    initializeContentTypes();

    int connfd;
    sockaddr_in serverAddress, clientAddress;
    socklen_t len = sizeof(sockaddr_in);
    char IPstring[INET_ADDRSTRLEN] = {0};

    /* initialize server socket */
    listenfd = socket(AF_INET, SOCK_STREAM, 0); /* SOCK_STREAM : TCP */
    if (listenfd < 0) {
        std::cout << "Error: initialization socket failed." << std::endl;
        return 0;
    }

    /* initialize server address (IP:port) */
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY; /* IP address: 0.0.0.0 */
    serverAddress.sin_port = htons(SERVER_PORT); /* port number */

    /* bind the socket to the server address */
    if (bind(listenfd, (sockaddr*) &serverAddress, sizeof(sockaddr)) < 0) {
        std::cout << "Error: binding failed." << std::endl;
        return 0;
    }

    if (listen(listenfd, LISTENNQ) < 0) {
        std::cout << "Error: cannot start listening." << std::endl;
        return 0;
    }

    /* keep processing incoming requests */
    while (1) {
        /* accept an incoming connection from the remote side */
        connfd = accept(listenfd, (struct sockaddr *)&clientAddress, &len);
        if (connfd < 0) {
            std::cout << "Error: failed to start a new connection to client." << std::endl;
            std::cout << "\tClient address: " << IPstring << std::endl;
            return 0;
        }

        /* print client (remote side) address (IP : port) */
        /* inet_ntop: converts the network address into a character std::string */
        /* https://www.systutorials.com/docs/linux/man/3-inet_ntop/ */
        inet_ntop(AF_INET, &(clientAddress.sin_addr), IPstring, INET_ADDRSTRLEN);
        std::cout << "Incoming connection from " << IPstring << ": " << ntohs(clientAddress.sin_port) << std::endl;
        std::cout << "Start new thread..." << std::endl;
        std::thread * t = new std::thread(handleRequest, connfd);
    }

    return 0;
}
