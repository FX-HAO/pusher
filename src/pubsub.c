#include "server.h"

void publishCommand(client *c) {
    int receivers = pubsubPublishMessage(c->argv[1],c->argv[2]);
    addReplyLongLong(c,receivers);
}
