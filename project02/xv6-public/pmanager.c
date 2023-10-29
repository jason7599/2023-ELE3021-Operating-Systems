#include "types.h"
#include "user.h"
#include "fcntl.h"

#define NBUF 100
#define FD 1

enum cmdtype { LIST, KILL, EXEC, MEML, EXIT, ERRR };
int expargc[5] = { 0, 1, 2, 2, 0 };

struct cmd
{
    enum cmdtype type;
    int argc;
    char *argv[NBUF];
};


int getline(char*, int);
struct cmd parsecmd(char*);


int main(void)
{
    static char buf[NBUF];
    struct cmd cmd;
    char *path;
    char *argv[2] = { 0 }; // exec 인자. "프로그램에 넘겨주는 인자는 하나로, 0번 인자에 path를 넣어줍니다."
    int pid, x;

    while (getline(buf, NBUF) >= 0)
    {
        if (!strlen(buf)) continue; // blank

        cmd = parsecmd(buf);

        if (cmd.type == ERRR)
        {
            printf(FD, "invalid command: %s\n", buf);
            continue;
        }
        if (cmd.argc != expargc[cmd.type])
        {
            printf(FD, "invalid argc: expected %d argument(s) but got %d\n", expargc[cmd.type], cmd.argc);
            continue;
        }

        switch (cmd.type)
        {
            case LIST:
                proclist(); 
                break;

            case KILL:
                pid = atoi(cmd.argv[0]);
                if (kill(pid)) printf(FD, "failed to kill %d\n", pid);
                else printf(FD, "sucessfully killed process %d\n", pid);
                break;

            case EXEC:
                path = cmd.argv[0];
                x = atoi(cmd.argv[1]); // stacksize
                argv[0] = path; // & argv[1]은 계속 0이여야됨. exec에서 argv 순회할때 0이 종료조건이기 때문

                // ! "실행한 프로그램의 종료를 기다리지 않고 pmanager는 이어서 실행되어야 합니다."
                // ! 그럼 여기서 바로 wait하면 안될듯. 
                // ! "자식 프로세스의 실행이 끝나도 해당 프로세스가 계속 zombie process로 남아있을 듯 한데,
                // !  pmanager process 삭제 시 orphan process가 처리되는 sequence에 따라 해결되도록 하여도 충분할까요? " => 아니오.
                // ! 그럼 pmanager가 종료될 때 reaping 하는 방법은 아니겠다. 
                // ! 시프때 본 건 SIGCHLD 같은 signal로 부모에게 끝났음을 알리는 방식인데 여긴 그런거 없는거같다

                // * * * double fork * * * * 
                // * 피아자에서 init이 zombie 출력하는건 정상이라 했듯이, 차라리 고아로 만들면 되겠음
                // * 이러려면 exec하는 녀석이 고아가 되어야 한다. 현재 디자인으론 exec한 애의 부모인 pmanager가 exit해야 고아가 됨
                // * pmanager-child1-child2 이런 식으로 fork를 두번하고, child2는 exec하고 부모인 child1이 exit하면 고아로 만들 수 있겠다.
                // * ㅇㅋ 그럼 init이 child2를 입양해서 처리, (이때 zombie! 뜨겠지) 그리고 pmanager는 exit한 고아메이커 child1을 ..
                // * 근데 이러면 또 pmanager가 child1을 wait하긴 해야하네; 바로 exit하니까 괜찮나
                // * ㅇㅇ child1이 바로 exit하면 문제 없을듯. 그리고 중요한건 child2의 수행이랑 독립되어야한다는거니까 이게 맞겠다

                // * 실험해본 결과 zombie 프로세스 안남고 잘 된다. 
                // * 원랜 pmanager exit할때 "zombie!" 떴는데 이젠 exec2 끝날때 "zombie!" 뜸. 이게 맞는듯

                pid = fork();
                if (pid == 0) // & child1: child2 만들고 도망가기
                {
                    pid = fork();
                    if (pid == 0) // & child2: exec2
                    {
                        exec2(path, argv, x); // should never return
                        printf(FD, "execute failed: %s\n", path);
                        // exec2 fail되면 자연스럽게 child2도 내려와서 아래 exit 호출하겠네 . 결국 어떻게든 고아가 되겠네
                    }
                    // & child1 먼저? exit으로 child2 고아 만들기.
                    // & child2가 먼저 끝날 수도 있나? 아 근데 먼저 끝났어도 어차피 wait 없이 exit했으니 고아되는건 마찬가지네 굿
                    exit(); 
                }

                wait(); // & pmanager: child1 wait하기. child1 바로 exit하니까 괜찮을 거 같다
                break;

            case MEML:
                pid = atoi(cmd.argv[0]);
                x = atoi(cmd.argv[1]); // limit
                if (setmemorylimit(pid, x)) printf(FD, "memlim failed\n");
                else printf(FD, "memlim successful\n");
                break;

            case EXIT:
                exit(); // * exec double fork로 바꿨으니 pmanager는 바로 exit해도 문제 없을거임

            default:
                printf(FD, "THIS SHOULD NEVER HAPPEN\n");
        }
    }
}


// * sh.c의 getcmd 좀 개조함
int getline(char *buf, int nbuf)
{
    printf(FD, "> ");
    memset(buf, 0, nbuf);
    gets(buf, nbuf);

    if(buf[0] == 0) // EOF?
        return -1;

    buf[strlen(buf) - 1] = 0; // 개행문자 잘라내기. 이거 안해주면 strlen 1 크게 나옴
    return 0;
}

// * line에서, 각 공백을 '\0'으로 바꾸고 그 다음 글자를 가리키는 포인터를 cmd의 배열에 저장
struct cmd parsecmd(char *line)
{
    struct cmd cmd;
    char *p;

    cmd.argc = 0;
    p = line;

    // 첫 word는 argv에 포함 x
    while ((p = strchr(p, ' '))) // 다음 공백 위치 찾고
    {
        line[p - line] = 0; // 그 인덱스를 \0으로 바꾸기
        cmd.argv[cmd.argc++] = ++p; // 공백 다음 글자부터 word 
    }

    // line 포인터가 첫번째 word 즉 cmdtype를 의미
    if (!strcmp(line, "list"))              cmd.type = LIST;
    else if (!strcmp(line, "kill"))         cmd.type = KILL;
    else if (!strcmp(line, "execute"))      cmd.type = EXEC;
    else if (!strcmp(line, "memlim"))       cmd.type = MEML;
    else if (!strcmp(line, "exit"))         cmd.type = EXIT;
    else                                    cmd.type = ERRR;

    return cmd;
}
