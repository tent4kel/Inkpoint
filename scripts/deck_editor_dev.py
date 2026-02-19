#!/usr/bin/env python3
"""
Dev server for DeckEditorPage.html — mock API, live reload on file change.
Usage: python3 scripts/deck_editor_dev.py [port]
"""
import http.server
import json
import os
import sys
import urllib.parse

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
HTML_FILE = os.path.join(os.path.dirname(__file__), "../src/network/html/DeckEditorPage.html")

MOCK_DECKS = [
    {"path": "/anki/Spanish.csv", "title": "Spanish"},
    {"path": "/anki/Japanese.csv", "title": "Japanese"},
    {"path": "/anki/Capitals.csv", "title": "Capitals"},
]

MOCK_CSVS = {
    "/anki/Spanish.csv": (
        "Front,Back,Repetitions,EasinessFactor,Interval,NextReviewSession\r\n"
        "Hola,Hello,3,2500,4,12\r\n"
        "\"Gracias\",\"Thank you\",1,2600,1,10\r\n"
        "\"¿Cómo estás?\",\"How are you?\",0,2500,0,0\r\n"
        "\"Buenos días\",\"Good morning\",5,2800,10,20\r\n"
        "Adiós,Goodbye,2,2400,2,9\r\n"
    ),
    "/anki/Japanese.csv": (
        "Front,Back,Repetitions,EasinessFactor,Interval,NextReviewSession\r\n"
        "犬,Dog,4,2600,8,18\r\n"
        "猫,Cat,2,2500,2,11\r\n"
        "水,Water,0,2500,0,0\r\n"
    ),
    "/anki/Capitals.csv": (
        "Front,Back,Repetitions,EasinessFactor,Interval,NextReviewSession\r\n"
        "France,Paris,6,2900,15,25\r\n"
        "Japan,Tokyo,4,2700,8,19\r\n"
        "Brazil,Brasília,1,2500,1,8\r\n"
        "\"United Kingdom\",London,5,2800,12,22\r\n"
    ),
}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"  {self.address_string()} {fmt % args}")

    def send_json(self, data, code=200):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

    def send_text(self, text, code=200, content_type="text/plain"):
        body = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)

        if path in ("/", "/deck-editor"):
            with open(HTML_FILE, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", len(body))
            self.end_headers()
            self.wfile.write(body)

        elif path == "/api/decks":
            self.send_json(MOCK_DECKS)

        elif path == "/api/deck":
            deck_path = qs.get("path", [None])[0]
            if not deck_path or deck_path not in MOCK_CSVS:
                self.send_text("Deck not found", 404)
            else:
                self.send_text(MOCK_CSVS[deck_path], content_type="text/plain")

        else:
            self.send_text("Not found", 404)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)

        if path == "/api/deck":
            deck_path = qs.get("path", [None])[0]
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length).decode("utf-8")
            if deck_path:
                MOCK_CSVS[deck_path] = body
                # Add to deck list if new
                if not any(d["path"] == deck_path for d in MOCK_DECKS):
                    title = deck_path.rsplit("/", 1)[-1].removesuffix(".csv")
                    MOCK_DECKS.append({"path": deck_path, "title": title})
                print(f"  [SAVE] {deck_path} ({len(body)} bytes)")
                self.send_text("OK")
            else:
                self.send_text("Missing path", 400)

        elif path == "/api/rename-deck":
            from_path = qs.get("from", [None])[0]
            to_path   = qs.get("to",   [None])[0]
            if not from_path or not to_path:
                self.send_text("Missing params", 400)
            elif not any(d["path"] == from_path for d in MOCK_DECKS):
                self.send_text("Not found", 404)
            elif any(d["path"] == to_path for d in MOCK_DECKS):
                self.send_text("Already exists", 409)
            else:
                new_title = to_path.rsplit("/", 1)[-1].removesuffix(".csv")
                for d in MOCK_DECKS:
                    if d["path"] == from_path:
                        d["path"] = to_path; d["title"] = new_title
                if from_path in MOCK_CSVS:
                    MOCK_CSVS[to_path] = MOCK_CSVS.pop(from_path)
                print(f"  [RENAME] {from_path} → {to_path}")
                self.send_text("OK")
        else:
            self.send_text("Not found", 404)


print(f"Deck Editor dev server → http://localhost:{PORT}/")
print(f"Serving: {os.path.abspath(HTML_FILE)}")
print("Press Ctrl+C to stop.\n")
http.server.HTTPServer(("", PORT), Handler).serve_forever()
