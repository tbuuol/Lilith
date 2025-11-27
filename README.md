# Lilith – Die Mutter des Kosmos

Lilith ist ein schlanker, kompromisslos direkter C++-RPC-Proxy für deine Blockchain-Knoten.
Kein Framework-Zirkus, keine Magie, nur pure, rohe Kontrolle.
Das Ding macht genau eine Sache – aber die macht es richtig:

Es nimmt lokale HTTP-Requests an → leitet deinen JSON-RPC Payload an den passenden Node weiter → liefert die Antwort zurück.
Mehr Minimalismus geht nur noch, wenn man es einfach lässt.

### Warum Lilith?

Weil Browser CORS hassen.
Und Nodes Browser hassen.
Und du keinen Bock auf PHP-Brücken, Bounce-Proxys oder Node.js-Mammut-Prozesse hast.

### Lilith löst genau das:

✔️ CORS-Preflights (OPTIONS) sauber beantworten  
✔️ POST-Payload direkt an deine Node weiterreichen  
✔️ Jeder Node läuft als eigenständige Verbindung (via Nakamoto)  
✔️ Jeder Client wird in einen eigenen Thread geworfen  
✔️ API bleibt blitzschnell & ohne Abhängigkeiten  



## Architektur – ultrakurz

# ApiServer

- Öffnet einen TCP-Socket
- Regelt CORS
- Verteilt Requests an Nodes
- Startet pro Client einen Thread (detached, du Tier!)


# Nakamoto

- Wrappt libcurl
- Baut stabile JSON-RPC Verbindungen
- Thread-safe dank Mutex
- Kein Blödsinn, nur straight-to-the-node



## Sicherheit?

Lilith läuft lokal.
If you expose this to the internet, Lilith wird dich höchstpersönlich ohrfeigen.
Extern → bitte Reverse Proxy + Auth davor.
Oder du lebst gefährlich, dein Karma.



## Was bringt dir das?

Du kannst Web-UIs bauen, die direkt mit deinen Nodes interagieren.

- Ohne Plugins.
- Ohne Browser-Hacks.
- Ohne extra Backend.

### Lilith ist die Brücke zwischen HTML/JS und deinem Node-Netzwerk.