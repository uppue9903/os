#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <cmath>
#include <pthread.h>
#include <atomic>
#include <algorithm>
#include <thread> 

using namespace std;
using namespace std::chrono;

int N, K, taskInc;
vector<vector<int>> sudoku;
vector<string> Buffers;
string output_filename = "output_BCAS.txt";

long long base_timestamp;
atomic<bool> sudokuInvalid(false);

// Maximum CAS retries before fallback
const int MAX_CAS_RETRIES = 10000;

atomic<int> task_rows(0);
atomic<int> task_cols(0);
atomic<int> task_subs(0);

enum TaskType
{
    ROW,
    COL,
    SUB,
    NONE
};


class thread_data
{
public:
    int thread_id;
    TaskType currentTask;
    int startIndex;
    int taskCount;
    long long total_cs_entry_time;
    long long total_cs_exit_time;
    long long worst_cs_entry;
    long long worst_cs_exit;
    int cs_count;

    thread_data()
    {
        thread_id = 0;
        currentTask = NONE;
        startIndex = 0;
        taskCount = 0;
        total_cs_entry_time = 0;
        total_cs_exit_time = 0;
        worst_cs_entry = 0;
        worst_cs_exit = 0;
        cs_count = 0;
    }
};


void writeOutputToFile(const string &output)
{
    ofstream outFile(output_filename, ios::app);
    if (outFile)
        outFile << output << endl;
    else
        cout << "Error: Could not open output file." << endl;
    outFile.close();
}

vector<string> splitByNewline(const string &buffer)
{
    vector<string> lines;
    int start = 0, end;
    while ((end = buffer.find('\n', start)) != string::npos)
    {
        string line = buffer.substr(start, end - start);
        if (!line.empty())
            lines.push_back(line);
        start = end + 1;
    }
    if (start < buffer.size())
        lines.push_back(buffer.substr(start));
    return lines;
}

void parseAndWriteLogs(const vector<string> &buffers)
{
    vector<pair<long long, string>> logs;
    for (const string &buffer : buffers)
    {
        vector<string> lines = splitByNewline(buffer);
        for (const string &line : lines)
        {
            size_t lastSpace = line.find_last_of(' ');
            if (lastSpace != string::npos)
            {
                string logMessage = line.substr(0, lastSpace);
                long long timestamp = stoll(line.substr(lastSpace + 1));
                logs.emplace_back(timestamp, logMessage);
            }
        }
    }
    sort(logs.begin(), logs.end(), [](const pair<long long, string> &a, const pair<long long, string> &b)
         { return a.first < b.first; });
    for (const auto &log : logs)
    {
        string s = log.second + " " + to_string(log.first);
        writeOutputToFile(s);
    }
}


bool get_work(thread_data *tdata)
{
    auto allocate_tasks = [&](atomic<int> &counter, TaskType taskType) -> bool
    {
        int current = counter.load();
        long long timestamp = duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count() - base_timestamp;
        if (current <= 0)
            return false;
        int allocated = min(taskInc, current);
        int retries = 0;
        while (!counter.compare_exchange_weak(current, current - allocated))
        {
            if (++retries > MAX_CAS_RETRIES)
            {
                int old = counter.fetch_sub(taskInc);
                allocated = min(taskInc, old);
                current = old;
                break;
            }
            if (sudokuInvalid.load())
                return false;
            this_thread::yield();
        }
        tdata->currentTask = taskType;
        tdata->taskCount = allocated;
        tdata->startIndex = N - current;
        string logMsg = "Thread " + to_string(tdata->thread_id) + " grabbed " + to_string(allocated) + " ";
        logMsg += (taskType == ROW ? "row" : (taskType == COL ? "column" : "subgrid"));
        logMsg += " tasks (counter updated) " + to_string(timestamp) + "\n";
        Buffers[tdata->thread_id] += logMsg;
        return true;
    };

    if (allocate_tasks(task_rows, ROW))
        return true;
    if (allocate_tasks(task_cols, COL))
        return true;
    if (allocate_tasks(task_subs, SUB))
        return true;
    return false;
}

bool rowCheck(int r)
{
    vector<int> row(N, 0);
    for (int j = 0; j < N; j++)
    {
        int val = sudoku[r][j];
        if (val < 1 || val > N || row[val - 1] == 1)
            return false;
        row[val - 1] = 1;
    }
    return true;
}

bool colCheck(int c)
{
    vector<int> col(N, 0);
    for (int i = 0; i < N; i++)
    {
        int val = sudoku[i][c];
        if (val < 1 || val > N || col[val - 1] == 1)
            return false;
        col[val - 1] = 1;
    }
    return true;
}

bool subCheck(int s)
{
    int n = sqrt(N);
    if (n * n != N)
    {
        cerr << "Error: N must be a perfect square." << endl;
        return false;
    }
    vector<int> sub(N, 0);
    int r = (s / n) * n;
    int c = (s % n) * n;
    for (int i = r; i < r + n; i++)
    {
        for (int j = c; j < c + n; j++)
        {
            int val = sudoku[i][j];
            if (val < 1 || val > N || sub[val - 1] == 1)
                return false;
            sub[val - 1] = 1;
        }
    }
    return true;
}

bool do_work(thread_data *tdata)
{
    int start = tdata->startIndex;
    int end = start + tdata->taskCount;
    for (int i = start; i < end; i++)
    {
        if (sudokuInvalid.load())
            return false;
        bool valid = false;
        long long timestamp = duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count() - base_timestamp;
        if (tdata->currentTask == ROW)
        {
            valid = rowCheck(i);
            if (!valid)
            {
                Buffers[tdata->thread_id] += "Thread " + to_string(tdata->thread_id) + " found error in row " +
                                             to_string(i) + " " + to_string(timestamp) + "\n";
                sudokuInvalid.store(true);
                return false;
            }
            else
            {
                Buffers[tdata->thread_id] += "Thread " + to_string(tdata->thread_id) + " validated row " +
                                             to_string(i) + " " + to_string(timestamp) + "\n";
            }
        }
        else if (tdata->currentTask == COL)
        {
            valid = colCheck(i);
            if (!valid)
            {
                Buffers[tdata->thread_id] += "Thread " + to_string(tdata->thread_id) + " found error in column " +
                                             to_string(i) + " " + to_string(timestamp) + "\n";
                sudokuInvalid.store(true);
                return false;
            }
            else
            {
                Buffers[tdata->thread_id] += "Thread " + to_string(tdata->thread_id) + " validated column " +
                                             to_string(i) + " " + to_string(timestamp) + "\n";
            }
        }
        else if (tdata->currentTask == SUB)
        {
            valid = subCheck(i);
            if (!valid)
            {
                Buffers[tdata->thread_id] += "Thread " + to_string(tdata->thread_id) + " found error in subgrid " +
                                             to_string(i) + " " + to_string(timestamp) + "\n";
                sudokuInvalid.store(true);
                return false;
            }
            else
            {
                Buffers[tdata->thread_id] += "Thread " + to_string(tdata->thread_id) + " validated subgrid " +
                                             to_string(i) + " " + to_string(timestamp) + "\n";
            }
        }
    }
    return true;
}

void *thdwork(void *param)
{
    thread_data *tdata = (thread_data *)param;
    while (true)
    {
        if (sudokuInvalid.load())
            break;
        auto cs_entry = high_resolution_clock::now();
        bool hasWork = get_work(tdata);
        auto cs_exit = high_resolution_clock::now();
        long long csEntryTime = duration_cast<nanoseconds>(cs_entry.time_since_epoch()).count() - base_timestamp;
        long long csExitTime = duration_cast<nanoseconds>(cs_exit.time_since_epoch()).count() - base_timestamp;
        tdata->total_cs_entry_time += csEntryTime;
        tdata->total_cs_exit_time += csExitTime;
        if (csEntryTime > tdata->worst_cs_entry)
            tdata->worst_cs_entry = csEntryTime;
        if (csExitTime > tdata->worst_cs_exit)
            tdata->worst_cs_exit = csExitTime;
        tdata->cs_count++;
        Buffers[tdata->thread_id] += "Thread " + to_string(tdata->thread_id) + " entered CS at " +
                                     to_string(csEntryTime) + " and exited at " + to_string(csExitTime) + "\n";
        if (!hasWork)
            break;
        if (!do_work(tdata))
            break;
    }
    pthread_exit(NULL);
    return NULL;
}

bool readInputFromFile(const string &filename)
{
    ifstream inputFile(filename);
    if (!inputFile)
    {
        cout << "Error: Could not open file " << filename << endl;
        return false;
    }
    inputFile >> K >> N >> taskInc;
    if (taskInc > N)
        taskInc = N;
    int sq_root = static_cast<int>(sqrt(N));
    if (sq_root * sq_root != N)
    {
        cout << "Error: N must be a perfect square." << endl;
        return false;
    }
    sudoku.resize(N, vector<int>(N));
    for (int i = 0; i < N; i++)
    {
        for (int j = 0; j < N; j++)
            inputFile >> sudoku[i][j];
    }
    inputFile.close();
    return true;
}

int main(int argc, char *argv[])
{
    if (!readInputFromFile(argv[1]))
    {
        return 1;
    }
    ofstream clearFile(output_filename, ios::out);
    clearFile.close();

    task_rows.store(N);
    task_cols.store(N);
    task_subs.store(N);

    Buffers.resize(K, "");

    auto start_time = high_resolution_clock::now();
    base_timestamp = duration_cast<nanoseconds>(start_time.time_since_epoch()).count();

    vector<thread_data *> tdata_arr;
    pthread_t threads[K];
    for (int i = 0; i < K; i++)
    {
        tdata_arr.push_back(new thread_data());
        tdata_arr[i]->thread_id = i;
    }
    for (int i = 0; i < K; i++)
    {
        pthread_create(&threads[i], NULL, thdwork, (void *)tdata_arr[i]);
    }
    for (int i = 0; i < K; i++)
    {
        pthread_join(threads[i], NULL);
    }

    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<nanoseconds>(end_time - start_time).count();
    parseAndWriteLogs(Buffers);

    string result = sudokuInvalid.load() ? "Sudoku is invalid.\n" : "Sudoku is valid.\n";
    writeOutputToFile(result);

    long long totalEntry = 0, totalExit = 0;
    long long worstEntry = 0, worstExit = 0;
    int totalCS = 0;
    for (int i = 0; i < K; i++)
    {
        totalEntry += tdata_arr[i]->total_cs_entry_time;
        totalExit += tdata_arr[i]->total_cs_exit_time;
        if (tdata_arr[i]->worst_cs_entry > worstEntry)
            worstEntry = tdata_arr[i]->worst_cs_entry;
        if (tdata_arr[i]->worst_cs_exit > worstExit)
            worstExit = tdata_arr[i]->worst_cs_exit;
        totalCS += tdata_arr[i]->cs_count;
    }
    long long avgEntry = (totalCS > 0) ? totalEntry / totalCS : 0;
    long long avgExit = (totalCS > 0) ? totalExit / totalCS : 0;



    writeOutputToFile("The total time taken is " + to_string(duration) + " nanoseconds.\n");
    writeOutputToFile("Average CS Entry Time is " + to_string(avgEntry) + " nanoseconds.\n");
    writeOutputToFile("Average CS Exit Time is " + to_string(avgExit) + " nanoseconds.\n");
    writeOutputToFile("Worst-case CS Entry Time is " + to_string(worstEntry) + " nanoseconds.\n");
    writeOutputToFile("Worst-case CS Exit Time is " + to_string(worstExit) + " nanoseconds.\n");

    cout << "The total time taken is " << duration << " nanoseconds." << endl;
    cout << "Average CS Entry Time is " << avgEntry << " nanoseconds." << endl;
    cout << "Average CS Exit Time is " << avgExit << " nanoseconds." << endl;
    cout << "Worst-case CS Entry Time is " << worstEntry << " nanoseconds." << endl;
    cout << "Worst-case CS Exit Time is " << worstExit << " nanoseconds." << endl;

    for (int i = 0; i < K; i++)
    {
        delete tdata_arr[i];
    }

    return 0;
}
