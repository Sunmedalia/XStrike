import socket, sys, time, threading

port = int(sys.argv[1])
ln = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
ln.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ln.bind(("127.0.0.1", port))
ln.listen(5)
print(f"[listener] bound 127.0.0.1:{port}", flush=True)

cycles = 0
while cycles < 3:
    try:
        s, _ = ln.accept()
    except OSError:
        break
    cycles += 1
    try:
        data = s.recv(4096)
        print(f"[listener] cycle {cycles}: received {data!r}", flush=True)
    except Exception as e:
        print(f"[listener] cycle {cycles}: recv error {e}", flush=True)
    # close immediately -> beacon should sleep ~1s and reconnect
    s.close()
    print(f"[listener] cycle {cycles}: closed, waiting for reconnect", flush=True)

ln.close()
print(f"[listener] saw {cycles} check-ins -> beacon reconnected {cycles-1}x", flush=True)
