// Nakamoto.cpp – vollständig auskommentierte, produktionsreife Version
#include "Nakamoto.hpp"
#include <iostream>                     // nur für Fehlermeldungen per std::cerr

// ====================================================================
// Anonymer Namespace → alles hier drin ist nur in dieser Datei sichtbar
// ====================================================================
namespace {

// writeCallback – wird von libcurl aufgerufen, sobald Daten vom Server kommen
// libcurl liefert die empfangenen Bytes in kleinen Chunks
// Parameter:
//   contents → Zeiger auf die empfangenen Bytes
//   size     → immer 1 (intern von libcurl)
//   nmemb    → Anzahl der Bytes in diesem Chunk
//   userp    → unser eigener Zeiger, den wir vorher mit CURLOPT_WRITEDATA gesetzt haben
size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    // Wir haben als userp einen Zeiger auf einen std::string übergeben
    auto* output = static_cast<std::string*>(userp);

    // Die empfangenen Bytes an den String anhängen
    // size * nmemb = tatsächliche Anzahl empfangener Bytes
    output->append(static_cast<char*>(contents), size * nmemb);

    // WICHTIG: Wir müssen genau die Anzahl der verarbeiteten Bytes zurückgeben
    return size * nmemb;
}

} // namespace

// ====================================================================
// Konstruktor – wird beim Erstellen jedes Nakamoto-Objekts aufgerufen
// ====================================================================
Nakamoto::Nakamoto(const std::string& user, const std::string& pass,
                   const std::string& host, int port)
    // Initialisiererliste – weist Member direkt zu (schneller + sicherer als im Rumpf)
    : rpcUser(user),                                   // RPC-Benutzername
      rpcPass(pass),                                   // RPC-Passwort
      rpcUrl("http://" + host + ":" + std::to_string(port)),  // Komplette URL einmalig bauen
      headers(nullptr),                                // Liste für HTTP-Header (noch leer)
      client(nullptr)                                  // CURL-Handle (noch nicht initialisiert)
{
    // libcurl global initialisieren (muss genau einmal pro Prozess passieren)
    // Wird normalerweise in main() gemacht – aber zur Sicherheit hier auch
    // (ist idempotent → mehrfach aufrufen schadet nicht)
    static bool curl_global_initialized = false;
    if (!curl_global_initialized) {
        curl_global_init(CURL_GLOBAL_ALL);
        curl_global_initialized = true;
    }

    // CURL-Handle erzeugen – das ist unser "HTTP-Client"
    client = curl_easy_init();
    if (!client) {
        std::cerr << "[Nakamoto] curl_easy_init() fehlgeschlagen!\n";
        return;                                        // Objekt bleibt "kaputt"
    }

    // ===== Feste Optionen – werden nur EINMAL gesetzt und bleiben für immer =====

    // Ziel-URL fest einstellen (wird bei jedem Request beibehalten)
    curl_easy_setopt(client, CURLOPT_URL, rpcUrl.c_str());

    // HTTP Basic Auth aktivieren
    curl_easy_setopt(client, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    // Benutzername:Passwort im Format "user:pass"
    curl_easy_setopt(client, CURLOPT_USERPWD, (rpcUser + ":" + rpcPass).c_str());

    // Unsere eigene Callback-Funktion für empfangene Daten
    curl_easy_setopt(client, CURLOPT_WRITEFUNCTION, writeCallback);

    // Verhindert, dass libcurl bei Verbindungsabbruch SIGPIPE sendet (kann Prozess killen)
    curl_easy_setopt(client, CURLOPT_NOSIGNAL, 1L);

    // Timeout: gesamter Request darf max. 15 Sekunden dauern
    curl_easy_setopt(client, CURLOPT_TIMEOUT, 15L);
    // Timeout nur für Verbindungsaufbau
    curl_easy_setopt(client, CURLOPT_CONNECTTIMEOUT, 5L);

    // HTTP-Header: Wir senden JSON
    headers = curl_slist_append(headers, "Content-Type: application/json");
    // Header-Liste an CURL übergeben
    curl_easy_setopt(client, CURLOPT_HTTPHEADER, headers);

    // Optional: Keep-Alive aktivieren (schnellere Folge-Requests)
    curl_easy_setopt(client, CURLOPT_TCP_KEEPALIVE, 1L);
}

// ====================================================================
// Destruktor – wird automatisch aufgerufen, wenn das Objekt zerstört wird
// ====================================================================
Nakamoto::~Nakamoto() {
    // HTTP-Header-Liste freigeben (wichtig, sonst Memory Leak!)
    if (headers) {
        curl_slist_free_all(headers);
        headers = nullptr;
    }

    // CURL-Handle sauber aufräumen
    if (client) {
        curl_easy_cleanup(client);
        client = nullptr;
    }
}

// ====================================================================
// WICHTIGSTE FUNKTION: Sende einen JSON-RPC-Request an die Node
// ====================================================================
std::string Nakamoto::sendRpc(const std::string& jsonPayload) {
    // Thread-Sicherheit: Mehrere Threads dürfen nicht gleichzeitig dasselbe CURL-Handle benutzen
    std::lock_guard<std::mutex> lock(rpcMutex);

    std::string response;                    // Hier wird die Antwort reingeschrieben
    if (!client) {
        // Falls Konstruktor fehlgeschlagen ist → früh abbrechen
        return response;
    }

    // === Nur die Dinge ändern, die sich pro Request ändern! ===
    // (Kein curl_easy_reset()! Das würde URL, Auth, Timeout usw. löschen!)

    response.clear();                        // Alten Inhalt löschen (wichtig!)

    // Den neuen JSON-Payload als POST-Daten setzen
    curl_easy_setopt(client, CURLOPT_POSTFIELDS, jsonPayload.c_str());
    // Explizite Länge angeben → verhindert, dass libcurl nach \0 sucht
    curl_easy_setopt(client, CURLOPT_POSTFIELDSIZE, jsonPayload.size());

    // Wohin soll die Antwort geschrieben werden? → in unseren response-String
    curl_easy_setopt(client, CURLOPT_WRITEDATA, &response);

    // === Request ausführen ===
    CURLcode res = curl_easy_perform(client);

    // === Fehlerbehandlung ===
    if (res != CURLE_OK) {
        std::cerr << "[Nakamoto] CURL-Fehler: " << curl_easy_strerror(res)
                  << " (URL: " << rpcUrl << ")\n";
        return response;                     // Leerer String bei Fehler
    }

    // HTTP-Statuscode prüfen (z.B. 401 = Auth-Fehler, 500 = Node kaputt)
    long http_code = 0;
    curl_easy_getinfo(client, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        std::cerr << "[Nakamoto] Node antwortet mit HTTP " << http_code
                  << " (URL: " << rpcUrl << ")\n";
        // Wir geben trotzdem die Antwort zurück – oft steht da ein JSON-Fehler drin
    }

    // Alles gut → Antwort zurückgeben
    return response;
}