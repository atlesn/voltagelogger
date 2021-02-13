#!/usr/bin/perl -w

package main;

use Socket qw(:DEFAULT :crlf inet_ntop);
use rrr::rrr_helper;
use rrr::rrr_helper::rrr_message;
use rrr::rrr_helper::rrr_settings;
use rrr::rrr_helper::rrr_debug;

my $debug = { };
bless $debug, rrr::rrr_helper::rrr_debug;

my $global_settings = undef;

sub process {
	my $message = shift;

	my $protocol = ($message->get_tag_all("http_protocol"))[0];

	if ($protocol == 2) {
		$debug->dbg(2, "Got HTTP2 request\n");
	}
	else {
		$debug->dbg(2, "Got HTTP1 request\n");
	}

	$message->clear_array();
	$message->send();

	return 1;
}

