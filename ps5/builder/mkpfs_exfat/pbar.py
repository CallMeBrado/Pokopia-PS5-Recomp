"""Stand-in for MkPFS's progress reporter: forwards byte counts to a callback."""


class Progress:
    def __init__(self, callback=None):
        self.callback = callback

    def step(self, phase, done, total, bytes_processed=0):
        if self.callback:
            self.callback(phase, done, total)
