// ApiServer.cpp – vollständig auskommentierte Minimal-Version
#include "ApiServer.hpp"       // Unsere eigene Header-Datei mit der Klassendefinition
#include "Nakamoto.hpp"        // Die Klasse, die die eigentliche RPC-Verbindung zur Node herstellt
#include <arpa/inet.h>         // Enthält htons(), INADDR_ANY usw. für Netzwerkadressen
#include <unistd.h>            // close(), read(), write(), usleep() etc.
#include <fcntl.h>             // Für fcntl() – hier zwar nicht benutzt, aber oft für non-blocking
#include <sys/socket.h>        // socket(), bind(), listen(), accept()
#include <netinet/in.h>        // sockaddr_in, in_addr Strukturen
#include <cstring>             // strlen(), memcpy(), memset() etc.
#include <sstream>             // std::ostringstream – um Strings einfach zusammenzubauen
#include <iostream>            // Nur für das eine cout beim Start
#include <thread>              // std::thread für asynchrone Client-Behandlung
#include <vector>              // Wird hier nicht benutzt, kann aber später z. B. für Thread-Pool

// Konstante: Wie viele Verbindungen maximal in der Warteschlange dürfen (backlog)
constexpr int BACKLOG = 8;

// ====================================================================
// Konstruktor – wird beim Erstellen eines ApiServer-Objekts aufgerufen
// ====================================================================
ApiServer::ApiServer() {
    // Beispiel-Nodes hart reinprogrammiert (Config-Datei weggelassen für Minimalität)
    // Schlüssel = Node-Name (wird später im JSON als "id" geschickt)
    // Wert     = ein Nakamoto-Objekt, das sich mit dieser Node verbindet
    nodes["Kotia"]    = std::make_unique<Nakamoto>("user", "pass", "127.0.0.1", 10001);
    nodes["Fairbrix"] = std::make_unique<Nakamoto>("user", "pass", "127.0.0.1", 10002);
    // Du kannst hier beliebig viele Nodes hinzufügen
}

// ====================================================================
// Destruktor – wird automatisch aufgerufen, wenn das Objekt zerstört wird
// ====================================================================
ApiServer::~ApiServer() {
    stop();                     // Sauberes Herunterfahren des Servers
}

// ====================================================================
// Öffentliche Methode: start() → startet den HTTP-Server (blockiert!)
// ====================================================================
void ApiServer::start() {
    // 1. Socket erzeugen: IPv4, TCP-Stream
    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {                     // Fehler beim Erzeugen?
        perror("socket");                      // Gibt Systemfehlermeldung aus
        return;                                // Abbruch – kein Server
    }

    // 2. Option setzen: Damit können wir den Port sofort wieder benutzen,
    //     auch wenn vorherige Instanz nicht sauber beendet wurde
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. Adressstruktur füllen
    sockaddr_in addr{};                        // alles mit 0 initialisieren
    addr.sin_family = AF_INET;                 // IPv4
    addr.sin_port   = htons(port);             // Port in Netzwerk-Byte-Order (Big-Endian)
    addr.sin_addr.s_addr = INADDR_ANY;         // Auf allen Interfaces lauschen (0.0.0.0)

    // 4. Socket an Port binden
    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_sock);
        return;
    }

    // 5. In den Listening-Modus schalten (wartet auf eingehende Verbindungen)
    if (listen(listen_sock, BACKLOG) < 0) {
        perror("listen");
        close(listen_sock);
        return;
    }

    running = true;                            // Flag: Server läuft jetzt
    std::cout << "[ApiServer] Listening on :" << port << '\n';

    run_loop();                                // Endlosschleife → blockiert hier
}

// ====================================================================
// Sauberes Stoppen des Servers (kann auch von außen aufgerufen werden)
// ====================================================================
void ApiServer::stop() {
    running = false;                           // Schleife in run_loop() beenden
    if (listen_sock >= 0) {                    // Falls Socket existiert
        shutdown(listen_sock, SHUT_RDWR);      // Keine neuen Verbindungen mehr + bestehende beenden
        close(listen_sock);                    // Socket schließen
        listen_sock = -1;                      // Kennzeichnen, dass er ungültig ist
    }
}

// ====================================================================
// Haupt-Event-Loop: wartet auf neue Verbindungen
// ====================================================================
void ApiServer::run_loop() {
    while (running) {                          // Solange der Server laufen soll
        // accept() wartet auf eine neue Client-Verbindung
        int client = accept(listen_sock, nullptr, nullptr);

        if (client < 0) {                      // Keine Verbindung da oder Fehler?
            if (errno == EINTR) continue;      // Unterbrechung durch Signal → nochmal versuchen
            // EWOULDBLOCK/EAGAIN würde hier kommen, wenn wir non-blocking accept benutzen würden
            continue;
        }

        // Neue Verbindung → eigenen Thread starten, der sich darum kümmert
        // detach() = Thread läuft unabhängig weiter, wir vergessen ihn
        std::thread(&ApiServer::handle_client, this, client).detach();
    }
}

// ====================================================================
// Hilfsfunktion: Liest die komplette HTTP-Anfrage bis zum doppelten \r\n\r\n
// (Header-Ende). Ohne das würde der Body abgeschnitten werden.
// ====================================================================
static std::string read_full_request(int sock) {
    std::string data;                          // Hier sammeln wir alles
    char buf[4096];                            // Puffer für read()
    while (data.find("\r\n\r\n") == std::string::npos) {  // Solange Header-Ende nicht da
        ssize_t n = read(sock, buf, sizeof(buf)); // Lies bis zu 4096 Bytes
        if (n <= 0) break;                     // Verbindung zu oder Fehler → abbrechen
        data.append(buf, n);                   // Anhängen der gelesenen Bytes
    }
    return data;                               // Kompletter Header (+ evtl. Body)
}

// ====================================================================
// Wichtigste Funktion: Verarbeitet eine einzelne Client-Anfrage
// ====================================================================
void ApiServer::handle_client(int client) {
    // 1. Komplette Anfrage einlesen
    std::string req = read_full_request(client);

    // 2. CORS Preflight (OPTIONS-Request) abfangen – Browser macht das vor POST
    if (req.rfind("OPTIONS ", 0) == 0) {       // Prüft, ob Request mit "OPTIONS " anfängt
        const char* resp =                     // Direkt fertige Antwort als C-String
            "HTTP/1.1 200 OK\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Content-Length: 0\r\n\r\n";       // Leere Antwort
        send(client, resp, strlen(resp), 0);   // Schickt Antwort
        close(client);                         // Fertig mit diesem Client
        return;                                // Weiteren Code überspringen
    }

    // 3. Body aus der Anfrage extrahieren (alles nach \r\n\r\n)
    size_t body_pos = req.find("\r\n\r\n");
    std::string body = (body_pos != std::string::npos) ? req.substr(body_pos + 4) : "";

    // 4. Standard-Fehlerantwort, falls nichts klappt
    std::string result = R"({"error":"invalid request","response":""})";

    // 5. Wenn ein Body da ist → JSON parsen und weiterleiten
    if (!body.empty()) {
        try {
            auto j = nlohmann::json::parse(body);        // Body als JSON parsen

            // "id" Feld aus JSON holen (z. B. "Kotia")
            // Falls nicht vorhanden → default "___" → find() schlägt fehl
            auto it = nodes.find(j.value("id", "___"));

            if (it != nodes.end()) {
                // Gefundene Node → komplette Anfrage weiterleiten
                result = it->second->sendRpc(body);
            } else {
                // Node nicht bekannt
                result = R"({"error":"node not found","response":""})";
            }
        } catch (...) {                                 // JSON kaputt oder sonstiger Fehler
            result = R"({"error":"json parse error","response":""})";
        }
    }

    // 6. HTTP-Antwort zusammenbauen
    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: application/json\r\n"
         << "Access-Control-Allow-Origin: *\r\n"      // CORS erlauben
         << "Content-Length: " << result.size() << "\r\n"
         << "Connection: close\r\n\r\n"               // Verbindung nach Antwort schließen
         << result;                                   // Eigentliche JSON-Antwort

    // 7. Antwort an Client schicken
    send(client, resp.str().c_str(), resp.str().size(), 0);

    // 8. Aufräumen
    close(client);                                      // Socket schließen
}