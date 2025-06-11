# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT
import make
import socket
import threading
import http.server
import socketserver
import fs_utils
import os
from args import args

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

class Dashboard:

  def __init__(self, recipe: make.Recipe):
    self.recipe = recipe
    self.exception = None
    self.port = None

  def start(self):
    self.port = find_available_port(8000)
    self.handler = http.server.SimpleHTTPRequestHandler

    try:
      self.httpd = socketserver.TCPServer(("", self.port), self.handler)
      self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
      self.thread.start()
    except Exception as e:
      self.exception = e
