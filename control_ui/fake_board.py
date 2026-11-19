#!/usr/bin/env python3
"""Board simulator for testing the UI without hardware.

Answers the board's UDP JSON API like the firmware does: every datagram
gets the /data JSON back, and servo moves follow the same speed ramp
(Q8 position, step = speed * 20 / 256 us per 20 ms frame, 255 = instant).

Run:  python fake_board.py           (listens on 127.0.0.1:6700)
Then: python server.py --board 127.0.0.1
"""

import argparse
import json
import socket
import time

SERVO_MAX = 3000
STEP_Q8_PER_SPEED = 20
FRAME_S = 0.02


class Servo:
    def __init__(self, center=1372):
        self.target = center
        self.position_q8 = center << 8
        self.speed = 255
        self.center = center

    def set_target(self, value):
        self.target = max(0, min(SERVO_MAX, int(value)))
        if self.speed == 255 or self.position_q8 == 0 or self.target == 0:
            self.position_q8 = self.target << 8

    def set_speed(self, value):
        self.speed = max(0, min(255, int(value)))
        if self.speed == 255:
            self.position_q8 = self.target << 8

    def step(self, frames):
        if self.speed == 0:
            return
        target_q8 = self.target << 8
        step = frames * self.speed * STEP_Q8_PER_SPEED
        if self.position_q8 < target_q8:
            self.position_q8 = min(target_q8, self.position_q8 + step)
        else:
            self.position_q8 = max(target_q8, self.position_q8 - step)

    @property
    def position(self):
        return self.position_q8 >> 8


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6700)
    args = parser.parse_args()

    servos = [Servo(), Servo()]
    started = time.monotonic()
    last_frame = started
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.host, args.port))
    sock.settimeout(FRAME_S)
    print(f"fake board on udp://{args.host}:{args.port}")

    while True:
        now = time.monotonic()
        frames = int((now - last_frame) / FRAME_S)
        if frames:
            last_frame += frames * FRAME_S
            for servo in servos:
                servo.step(frames)
        try:
            data, addr = sock.recvfrom(1024)
        except socket.timeout:
            continue
        try:
            request = json.loads(data or b"{}")
        except json.JSONDecodeError:
            request = {}
        for index, servo in enumerate(servos, start=1):
            if f"servo{index}_speed" in request:
                servo.set_speed(request[f"servo{index}_speed"])
            if f"servo{index}" in request:
                servo.set_target(request[f"servo{index}"])
        reply = {
            "uptime_s": int(now - started),
            "pwm": {
                "servo1": servos[0].target, "servo2": servos[1].target,
                "servo1_speed": servos[0].speed, "servo2_speed": servos[1].speed,
                "servo1_pos": servos[0].position, "servo2_pos": servos[1].position,
                "servo1_center": servos[0].center, "servo2_center": servos[1].center,
            },
        }
        sock.sendto((json.dumps(reply, separators=(",", ":")) + "\n").encode(), addr)


if __name__ == "__main__":
    main()
