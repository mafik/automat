# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT
import make
import socket
import threading
import http.server
import socketserver
import fs_utils
import os
import json
from pathlib import Path
from args import args
from functools import partial
import graph_printer
import make

def find_available_port(start_port=8000):
  """Find an available port starting from start_port."""
  for port in range(start_port, start_port + 100):
    try:
      with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('localhost', port))
        return port
    except OSError:
      continue
  return None

class AutomatRequestHandler(http.server.SimpleHTTPRequestHandler):
  """Custom HTTP request handler that serves graph dynamically at root path."""

  def __init__(self, *args, recipe=None, html='??', **kwargs):
    self.recipe = recipe
    self.html = html
    super().__init__(*args, **kwargs)

  def do_GET(self):
    if self.path == '/' or self.path == '/index.html':
      self.serve_graph()
    elif self.path.startswith('/run_py/'):
      # Serve static files from run_py directory (CSS, JS)
      self.serve_static_file()
    else:
      # For other paths, use default behavior
      super().do_GET()

  def serve_graph(self):
    """Generate and serve the graph HTML dynamically."""
    try:
      self.send_response(200)
      self.send_header('Content-type', 'text/html; charset=utf-8')
      self.send_header('Content-Length', str(len(self.html.encode('utf-8'))))
      self.end_headers()
      self.wfile.write(self.html.encode('utf-8'))
    except Exception as e:
      self.send_error(500, f"Error generating graph: {str(e)}")
      print(e)
      raise e

  def serve_static_file(self):
    """Serve static files from the run_py directory."""
    # Remove the leading /run_py/ from the path
    file_path = self.path[8:]  # Remove '/run_py/'
    static_file_path = fs_utils.project_root / 'run_py' / file_path

    if static_file_path.exists() and static_file_path.is_file():
      try:
        with open(static_file_path, 'rb') as f:
          content = f.read()

        # Determine content type based on file extension
        content_type = 'text/plain'
        if file_path.endswith('.css'):
          content_type = 'text/css'
        elif file_path.endswith('.js'):
          content_type = 'application/javascript'

        self.send_response(200)
        self.send_header('Content-type', content_type)
        self.send_header('Content-Length', str(len(content)))
        self.end_headers()
        self.wfile.write(content)
      except Exception as e:
        self.send_error(500, f"Error serving static file: {str(e)}")
    else:
      self.send_error(404, "File not found")

class Dashboard:

  def __init__(self, recipe: make.Recipe):
    self.recipe = recipe
    self.exception = None
    self.port = None

  def start(self):
    self.port = find_available_port(8000)
    if self.port is None:
      self.exception = Exception("No available port found")
      return

    # Generate the HTML at the startup because the recipe is going to be pruned by `set_target`.
    html = graph_printer.print_graph(self.recipe)

    try:
      self.httpd = socketserver.TCPServer(("", self.port), partial(AutomatRequestHandler, recipe=self.recipe, html=html))
      self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
      self.thread.start()
    except Exception as e:
      self.exception = e
