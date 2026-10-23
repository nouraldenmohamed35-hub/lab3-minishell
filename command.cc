
/*
 * 
 *
 * Template file.
 * You will need to add more code here to execute the command table.
 *
 * NOTE: You are responsible for fixing any bugs this code may have!
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <glob.h>
#include <wordexp.h>
#include <vector>
#include <string>
#include <cctype>
#include <iostream>
#include <sstream>
#include "command.h"
#include "tokenizer.h"

// add a word to the command; if it has * or ? replace it with the matching file names
static void addWord(SimpleCommand *cmd, const std::string &word)
{
    if (word.find_first_of("*?") != std::string::npos)
    {
        glob_t result;
        if (glob(word.c_str(), 0, NULL, &result) == 0)
        {
            for (size_t i = 0; i < result.gl_pathc; i++)
                cmd->insertArgument(strdup(result.gl_pathv[i]));
            globfree(&result);
            return;
        }
        globfree(&result);
    }

    // normal word (or no file matched)
    cmd->insertArgument(strdup(word.c_str()));
}

static int g_logFd = -1;

// write one line in the log file
static void writeLog(pid_t pid)
{
    char line[100];
    int len = snprintf(line, sizeof(line), "Child process %d terminated\n", (int)pid);
    write(g_logFd, line, len);
}

// called when a child process ends
static void childEnded(int sig)
{
    (void)sig;
    int savedErrno = errno;
    pid_t pid;

    // collect every child that has finished
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
        writeLog(pid);

    errno = savedErrno;
}


// does the text end with this character?
static bool endsWith(const std::string &text, char c)
{
    return !text.empty() && text.back() == c;
}

// The tokenizer only splits on spaces, so "Hello, World!" comes as 2 tokens:
//     "Hello,     and     World!"
// This function puts them together and removes the quotes.
// It returns false if a quote is never closed.
static bool mergeQuotes(std::vector<Token> &tokens)
{
    std::vector<Token> result;
    size_t i = 0;

    while (i < tokens.size())
    {
        Token t = tokens[i];
        char quote = t.value.empty() ? 0 : t.value[0];
        bool isWord = (t.type == TOKEN_COMMAND || t.type == TOKEN_ARGUMENT);

        // a normal token: keep it as it is
        if (!isWord || (quote != '"' && quote != '\''))
        {
            result.push_back(t);
            i++;
            continue;
        }

        // a quoted text: remove the first quote
        std::string text = t.value.substr(1);
        bool closed = endsWith(text, quote);

        // add the next words until we find the last quote
        // (the last token is EOF, so we stop before it)
        while (!closed && i + 1 < tokens.size() - 1)
        {
            i++;
            text += " " + tokens[i].value;
            closed = endsWith(text, quote);
        }

        if (!closed)
        {
            fprintf(stderr, "myshell: syntax error: unterminated quote\n");
            return false;
        }

        text.pop_back(); // remove the last quote
        t.value = text;
        result.push_back(t);
        i++;
    }

    tokens = result;
    return true;
}

static void setString(char **dest, const std::string &value)
{
    if (*dest)
        free(*dest);
    *dest = strdup(value.c_str());
}




// is this token a normal word (a command name or an argument)?
static bool isWord(const Token &t)
{
    return t.type == TOKEN_COMMAND || t.type == TOKEN_ARGUMENT;
}

// is this token one of  >  >>  <  2>  >>&  ?
static bool isRedirect(TokenType type)
{
    return type == TOKEN_REDIRECT || type == TOKEN_APPEND ||
           type == TOKEN_INPUT || type == TOKEN_ERROR ||
           type == TOKEN_REDIRECT_AND_ERROR;
}

// Handle  >  >>  <  2>  >>&
// tokens[i] is the sign and tokens[i + 1] is the file name.
// It returns an error message, or "" if everything is fine.
static std::string handleRedirect(Command &cmd, std::vector<Token> &tokens, size_t &i)
{
    Token &sign = tokens[i];
    Token &file = tokens[i + 1]; // the last token is EOF, so i + 1 exists

    if (!isWord(file))
        return "missing file name after " + sign.value;

    i++; // skip the file name

    if (sign.type == TOKEN_INPUT) // <
    {
        if (cmd._inputFile)
            return "duplicate input redirection";
        if (cmd._numberOfSimpleCommands > 0)
            return "ambiguous input redirection (only the first command of a pipe can read from a file)";

        setString(&cmd._inputFile, file.value);
    }
    else if (sign.type == TOKEN_ERROR) // 2>
    {
        if (cmd._errFile)
            return "duplicate error redirection";

        setString(&cmd._errFile, file.value);
    }
    else // >   >>   >>&
    {
        if (cmd._outFile)
            return "duplicate output redirection";

        setString(&cmd._outFile, file.value);
        cmd._append = (sign.type == TOKEN_REDIRECT) ? 0 : 1;

        if (sign.type == TOKEN_REDIRECT_AND_ERROR) // the errors go to the same file
        {
            if (cmd._errFile)
                return "duplicate error redirection";

            setString(&cmd._errFile, file.value);
            cmd._out_error = 1;
        }
    }

    return "";
}

// Handle  |   (the command before the pipe is finished, so we add it)
// It returns an error message, or "" if everything is fine.
static std::string handlePipe(Command &cmd, SimpleCommand *simple)
{
    if (simple->_numberOfArguments == 0)
        return "missing command before |";
    if (cmd._outFile)
        return "ambiguous output redirection (only the last command of a pipe can write to a file)";

    cmd.insertSimpleCommand(simple);
    return "";
}

// Handle the end of the line (the EOF token): add the last command
// It returns an error message, or "" if everything is fine.
static std::string handleEnd(Command &cmd, SimpleCommand *simple, bool afterPipe)
{
    if (simple->_numberOfArguments > 0)
    {
        cmd.insertSimpleCommand(simple);
        return "";
    }
    if (afterPipe)
        return "missing command after |";
    if (cmd._numberOfSimpleCommands == 0)
        return "missing command";

    delete simple;
    return "";
}


void parse(std::vector<Token> &tokens)
{
    // empty line: only the EOF token
    if (tokens.size() <= 1)
        return;

    // put the words of a quoted text together
    if (!mergeQuotes(tokens))
        return;

    Command currentCommand;
    SimpleCommand *currentSimpleCommand = new SimpleCommand();
    bool afterPipe = false; // true if the last token was a pipe
    std::string error;      // stays empty if the line is correct

    for (size_t i = 0; i < tokens.size() && error.empty(); i++)
    {
        Token &t = tokens[i];

        if (isWord(t))
        {
            addWord(currentSimpleCommand, t.value);
            afterPipe = false;
        }
        else if (isRedirect(t.type))
        {
            error = handleRedirect(currentCommand, tokens, i);
        }
        else if (t.type == TOKEN_PIPE)
        {
            error = handlePipe(currentCommand, currentSimpleCommand);
            if (error.empty())
            {
                currentSimpleCommand = new SimpleCommand(); // the next command
                afterPipe = true;
            }
        }
        else if (t.type == TOKEN_BACKGROUND)
        {
            if (tokens[i + 1].type != TOKEN_EOF)
                error = "& must be the last token on the line";
            else
                currentCommand._background = 1;
        }
        else if (t.type == TOKEN_EOF)
        {
            error = handleEnd(currentCommand, currentSimpleCommand, afterPipe);
        }
    }

    // wrong line: print the error and throw everything away
    if (!error.empty())
    {
        fprintf(stderr, "myshell: syntax error: %s\n", error.c_str());
        currentCommand.insertSimpleCommand(currentSimpleCommand); // so clear() frees it too
        currentCommand.clear();
        return;
    }

    currentCommand.execute();
}

SimpleCommand::SimpleCommand()
{
    _numberOfAvailableArguments = 5;
    _numberOfArguments = 0;
    _arguments = (char **)malloc(_numberOfAvailableArguments * sizeof(char *));
}

void SimpleCommand::insertArgument(char *argument)
{
    if (_numberOfAvailableArguments == _numberOfArguments + 1)
    {
        _numberOfAvailableArguments *= 2;
        _arguments = (char **)realloc(_arguments,
                                      _numberOfAvailableArguments * sizeof(char *));
    }

    _arguments[_numberOfArguments] = argument;
    _arguments[_numberOfArguments + 1] = NULL;

    _numberOfArguments++;
}

Command::Command()
{
    _numberOfAvailableSimpleCommands = 1;
    _numberOfSimpleCommands = 0;
    _simpleCommands = (SimpleCommand **)
        malloc(_numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));
    _outFile = 0;
    _inputFile = 0;
    _errFile = 0;
    _out_error = 0;
    _background = 0;
    _append = 0;
}
void Command::insertSimpleCommand(SimpleCommand *simpleCommand)
{
    if (_numberOfAvailableSimpleCommands == _numberOfSimpleCommands)
    {
        _numberOfAvailableSimpleCommands *= 2;
        _simpleCommands = (SimpleCommand **)realloc(_simpleCommands,
                                                    _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));
    }

    _simpleCommands[_numberOfSimpleCommands] = simpleCommand;
    _numberOfSimpleCommands++;
}

void Command::clear()
{
    for (int i = 0; i < _numberOfSimpleCommands; i++)
    {
        for (int j = 0; j < _simpleCommands[i]->_numberOfArguments; j++)
        {
            free(_simpleCommands[i]->_arguments[j]);
        }

        free(_simpleCommands[i]->_arguments);
        free(_simpleCommands[i]);
    }

    if (_outFile)
    {
        free(_outFile);
    }

    if (_inputFile)
    {
        free(_inputFile);
    }

    if (_errFile)
    {
        free(_errFile);
    }

    _numberOfSimpleCommands = 0;
    _outFile = 0;
    _inputFile = 0;
    _errFile = 0;
    _background = 0;
    _out_error = 0;
    _append = 0;
}

void Command::print()
{
    printf("\n\n");
    printf("              COMMAND TABLE                \n");
    printf("\n");
    printf("  #   Simple Commands\n");
    printf("  --- ----------------------------------------------------------\n");

    for (int i = 0; i < _numberOfSimpleCommands; i++)
    {
        if (i > 0) printf("\n");
        printf("  %-3d ", i);
        for (int j = 0; j < _simpleCommands[i]->_numberOfArguments; j++)
        {
            printf("\"%s\" \t", _simpleCommands[i]->_arguments[j]);
        }
    }

    printf("\n\n");
    printf("  Output       Input        Error        Err&Out       Background\n");
    printf("  ------------ ------------ ------------ ------------ ------------\n");
    printf("  %-12s %-12s %-12s %-12s %-12s\n", _outFile ? _outFile : "default",
           _inputFile ? _inputFile : "default", _errFile ? _errFile : "default", _out_error == 1 ? _errFile : "default"
           ,_background ? "YES" : "NO");
    printf("\n\n");
}

// The next 3 functions are used inside the child.
// Each one opens a file and uses it as the input, the output or the errors.

// < file
static void redirectInput(const char *file)
{
    int f = open(file, O_RDONLY);
    if (f < 0)
    {
        perror(file);
        _exit(1);
    }
    dup2(f, 0);
    close(f);
}

// > file   or   >> file   or   >>& file
static void redirectOutput(const char *file, int append, int alsoErrors)
{
    int flags = O_WRONLY | O_CREAT;
    if (append)
        flags = flags | O_APPEND;
    else
        flags = flags | O_TRUNC;

    int f = open(file, flags, 0644);
    if (f < 0)
    {
        perror(file);
        _exit(1);
    }
    dup2(f, 1);

    // for >>& the errors go to the same file
    if (alsoErrors)
        dup2(f, 2);

    close(f);
}

// 2> file
static void redirectErrors(const char *file)
{
    int f = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0)
    {
        perror(file);
        _exit(1);
    }
    dup2(f, 2);
    close(f);
}

// exit and cd (the shell does them itself, no fork)
// returns true if the command was exit or cd
static bool runBuiltin(char **args)
{
    // exit
    if (strcmp(args[0], "exit") == 0)
    {
        printf("\n    Good bye!!\n\n");
        exit(0);
    }

    // cd
    if (strcmp(args[0], "cd") == 0)
    {
        // no folder means go home
        const char *dir = args[1];
        if (dir == NULL)
            dir = getenv("HOME");

        if (dir == NULL)
            fprintf(stderr, "myshell: cd: HOME is not set\n");
        else if (chdir(dir) < 0)
            perror(dir);

        return true;
    }

    return false;
}

void Command::execute()
{
    // show the command table
    print();
    fflush(stdout);

    pid_t lastPid = -1;
    int prevRead = -1; // read end of the previous pipe

    // exit and cd
    if (_numberOfSimpleCommands == 1 && runBuiltin(_simpleCommands[0]->_arguments))
    {
        clear();
        return;
    }
    for (int i = 0; i < _numberOfSimpleCommands; i++)
    {
        char **args = _simpleCommands[i]->_arguments;
        bool isFirst = (i == 0);
        bool isLast = (i == _numberOfSimpleCommands - 1);

        // make a pipe to the next command (the last one does not need it)
        int fd[2] = {-1, -1};
        if (!isLast)
        {
            if (pipe(fd) < 0)
            {
                perror("myshell: pipe");
                break;
            }
        }

        // make a new process
        pid_t pid = fork();

        if (pid < 0)
        {
            perror("myshell: fork");
            break;
        }

        if (pid == 0)
        {
            // the commands we run can still be stopped with Ctrl-C
            signal(SIGINT, SIG_DFL);

            // take input from the previous command
            if (prevRead != -1)
                dup2(prevRead, 0);

            // send output to the next command
            if (!isLast)
                dup2(fd[1], 1);

            // close the pipe ends, we do not need them any more
            if (prevRead != -1)
                close(prevRead);
            if (!isLast)
            {
                close(fd[0]);
                close(fd[1]);
            }

            // input file (first command only)
            if (_inputFile && isFirst)
                redirectInput(_inputFile);

            // output file (last command only)
            if (_outFile && isLast)
                redirectOutput(_outFile, _append, _out_error);

            // error file
            if (_errFile && !_out_error)
                redirectErrors(_errFile);

            // run the command
            execvp(args[0], args);

            // if we are here, execvp failed
            fprintf(stderr, "myshell: %s: command not found\n", args[0]);
            _exit(1);
        }

        // parent: close the pipe ends we do not need
        if (prevRead != -1)
            close(prevRead);
        if (!isLast)
        {
            close(fd[1]);
            prevRead = fd[0];
        }

        lastPid = pid;
    }

    // wait for the last command (not for background commands)
    if (!_background && lastPid > 0)
    {
        if (waitpid(lastPid, NULL, 0) == lastPid)
            writeLog(lastPid); // write it in the log file
    }
    clear();
}

void Command::prompt()
{
    std::string input;
    while (true)
    {
        printf("myshell> ");
        fflush(stdout);
        if (!std::getline(std::cin, input)) // Ctrl+D
        {
            printf("\n");
            exit(0);
        }
        std::vector<Token> tokens = tokenize(input);
        parse(tokens);
    }
}

Command Command::_currentCommand;
SimpleCommand *Command::_currentSimpleCommand;

int main()
{
    // open the log file and set the SIGCHLD handler
    g_logFd = open("myshell.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    struct sigaction sa;
    sa.sa_handler = childEnded;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    // the shell ignores Ctrl-C
    signal(SIGINT, SIG_IGN);
    Command::_currentCommand.prompt();
    return 0;
}
