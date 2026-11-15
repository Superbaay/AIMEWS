"""Servidor estático de sitio_web sin caché (Cloudflare y el navegador siempre piden la versión actual)."""
import functools
import http.server
import sys

carpeta = sys.argv[1]
puerto = int(sys.argv[2]) if len(sys.argv) > 2 else 5500


class manejador_sin_cache(http.server.SimpleHTTPRequestHandler):
    def redirigir_a_https(self):
        # Cloudflare avisa el esquema original; con http el login falla por CORS, así que se manda a https
        if self.headers.get("X-Forwarded-Proto") != "http":
            return False
        self.send_response(301)
        self.send_header("Location", f"https://{self.headers.get('Host', '')}{self.path}")
        self.send_header("Content-Length", "0")
        self.end_headers()
        return True

    def do_GET(self):
        if not self.redirigir_a_https():
            super().do_GET()

    def do_HEAD(self):
        if not self.redirigir_a_https():
            super().do_HEAD()

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


servidor = http.server.ThreadingHTTPServer(("127.0.0.1", puerto), functools.partial(manejador_sin_cache, directory=carpeta))
print(f"Sirviendo {carpeta} en http://127.0.0.1:{puerto} sin caché", flush=True)
servidor.serve_forever()
