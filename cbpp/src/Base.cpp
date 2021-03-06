
#include "Base.h"

#include <string>


#ifdef _WIN32
#define WIN_32_LEAN_AND_MEAN
#include <windows.h>
#endif


#ifdef _WIN32

std::string GetCurrentWorkingDirectory()
{
	char buffer[512];
	GetCurrentDirectory(sizeof(buffer) - 1, buffer);
	return buffer;
}

std::string GetExecutableFullPath()
{
	char buffer[512];
	GetModuleFileName(NULL, buffer, sizeof(buffer) - 1);
	return buffer;
}

#endif


std::string GetPathDrive(const std::string& path)
{
	if (path.length() > 1 && path[1] == ':')
		return path.substr(0, 2);
	return "";
}


std::string GetPathDirectory(const std::string& path)
{
	// Parse the executable path looking for its directory
	std::string directory = "";
	size_t sep = path.rfind('\\');
	if (sep == -1)
		sep = path.rfind('/');
	if (sep != -1)
		directory = path.substr(0, sep);
	return directory;
}


std::string JoinPaths(const std::string& p0, const std::string& p1)
{
	// Need to take into account "A"+"B", "A/"+"B", "A"+"/B", "A/"+"/B"
	std::string path = p0;
	if (p0.back() == '\\' || p0.back() == '/')
		path = path.substr(0, path.length() - 1);
	if (p1.front() != '\\' && p1.back() != '/')
		path += '/';
	path += p1;
	return path;
}


bool IsPathAbsolute(const std::string& path)
{
	size_t len = path.length();
	if (len == 0)
		return false;

	// Start with a single backslash is absolute for current drive
	if (path[0] == '\\')
		return true;

	// Drive specified
	if (len > 1 && path[1] == ':')
		return true;

	return false;
}


File::File()
	: fp(0)
{
}


File::~File()
{
	if (fp != 0)
		fclose(fp);
}


bool Open(File& file, const char* filename, const char* mode)
{
	// Close any old file before opening a new one
	if (file.fp)
		fclose(file.fp);
	file.fp = fopen(filename, mode);
	return file.fp != 0;
}


size_t Size(const File& file)
{
	if (file.fp == 0)
		return 0;

	// Seek to EOF and get position
	if (fseek(file.fp, 0, SEEK_END) != 0)
		return 0;
	size_t size = ftell(file.fp);
	if (fseek(file.fp, 0, SEEK_SET) != 0)
		return 0;

	return size;
}


bool Read(const File& file, void* dest, size_t size)
{
	if (file.fp == 0)
		return false;
	return fread(dest, 1, size, file.fp) == size;
}


bool LoadFileData(const char* filename, std::vector<char>& file_data)
{
	File file;
	if (!Open(file, filename, "rb"))
		return false;

	file_data.resize(Size(file));
	return Read(file, file_data.data(), file_data.size());
}


Arguments::Arguments(int argc, const char* argv[])
{
	// Copy from the command-line into local storage
	m_Arguments.resize(argc);
	for (size_t i = 0; i < m_Arguments.size(); i++)
		m_Arguments[i] = argv[i];

	// Override first argument with complete path to executable
	m_Arguments[0] = GetExecutableFullPath();
}


size_t Arguments::GetIndexOf(const std::string& arg, int occurrence) const
{
	// Linear search for a matching argument
	int found = 0;
	for (size_t i = 0; i < m_Arguments.size(); i++)
	{
		if (m_Arguments[i] == arg)
		{
			if (found++ == occurrence)
				return i;
		}
	}

	return -1;
}


bool Arguments::Have(const std::string& arg) const
{
	// Does the specific argument exist?
	return GetIndexOf(arg) != -1;
}


std::string Arguments::GetProperty(const std::string& arg, int occurrence) const
{
	// Does the arg exist and does it have a value
	size_t index = GetIndexOf(arg, occurrence);
	if (index == -1 || index + 1 >= m_Arguments.size())
		return "";

	return m_Arguments[index + 1];
}


size_t Arguments::Count() const
{
	return m_Arguments.size();
}


const std::string& Arguments::operator [] (int index) const
{
	return m_Arguments[index];
}
