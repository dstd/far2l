#pragma once
#include <stdexcept>
#include <vector>
#include <WinCompat.h>

class TTYWriter
{
	enum { AUTO_FLUSH_THRESHOLD = 0x1000 };

	struct Cursor
	{
		unsigned int y = -1, x = -1;
		bool visible = false;
		unsigned char height = 0;
	} _cursor;

	struct Attributes
	{
		Attributes() = default;
		Attributes(const Attributes &) = default;
		Attributes(WORD attributes);

		bool bold = false;
		bool underline = false;
		unsigned char foreground = -1;
		unsigned char background = -1;

		bool operator ==(const Attributes &attr) const;
		bool operator !=(const Attributes &attr) const {return !(operator ==(attr)); }
	} _attr;

	std::vector<char> _rawbuf;
	void WriteReally(const char *str, int len);

	void Write(const char *str, int len);
	void Format(const char *fmt, ...);
public:

	void Flush();
	void SetScreenBuffer(bool alternate);

	void ChangeCursor(bool visible, unsigned char height, bool force = false);
	void MoveCursor(unsigned int y, unsigned int x, bool force = false);
	void WriteLine(const CHAR_INFO *ci, unsigned int cnt);
};
