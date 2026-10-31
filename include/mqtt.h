#pragma once

// Send message to AWS
int aws_mqtt_send(const char *payload, const char *payload2);

// Receive a message from AWS
char *aws_mqtt_get_message(void);
