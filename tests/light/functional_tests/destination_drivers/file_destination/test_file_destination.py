#!/usr/bin/env python
import pytest
import glob
from time import sleep


def test_file_destination(config, syslog_ng):
    counter = 1000
    message = "message text"

    file_name = "temp-test.log"
    generator_source = config.create_example_msg_generator_source(num=counter, freq=0.0001, template=config.stringify(message))
    file_destination = config.create_file_destination(file_name=file_name, logrotate="enable(yes), rotate(5), size(10000)")
    
    config.create_logpath(statements=[generator_source, file_destination])
    syslog_ng.start(config)

    sleep(1)

    log_file_list = glob.glob(file_name + "*")
    total_log_count = 0
    for file in log_file_list:
       f = open(file)
       total_log_count += sum(1 for _ in f) 
       f.close()
    
    assert (total_log_count == counter)