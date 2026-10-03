# voice_prompts/

Where `firmware/bringup/gen_prompt.py` saves the spoken prompts it generates
(`operator.wav`, `record_prompt.wav`, ...) before uploading them to a board.
The WAVs are git-ignored: they're rebuilt from each prompt's text any time.
