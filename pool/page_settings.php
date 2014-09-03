<?php
#
function settings($data, $user, $email, $addr, $err)
{
 $pg = '<h1>Account Settings</h1>';

 if ($err != '')
	$pg .= "<span class=err>$err<br><br></span>";

 $pg .= '<table cellpadding=20 cellspacing=0 border=1>';
 $pg .= '<tr class=dc><td><center>';

 $pg .= makeForm('settings');
 $pg .= '<table cellpadding=5 cellspacing=0 border=0>';
 $pg .= '<tr class=dc><td class=dr colspan=2>';
 $pg .= 'To change your email, enter a new email address and your password';
 $pg .= '</td></tr>';
 $pg .= '<tr class=dc><td class=dr>';
 $pg .= 'EMail:';
 $pg .= '</td><td class=dl>';
 $pg .= "<input type=text name=email value='$email' size=20>";
 $pg .= '</td></tr>';
 $pg .= '<tr class=dc><td class=dr>';
 $pg .= 'Password:';
 $pg .= '</td><td class=dl>';
 $pg .= '<input type=password name=pass size=20>';
 $pg .= '</td></tr>';
 $pg .= '<tr class=dc><td class=dr colspan=2>';
 $pg .= 'Change: <input type=submit name=Change value=EMail>';
 $pg .= '</td></tr>';
 $pg .= '</table></form>';

 $pg .= '</center></td></tr>';
 $pg .= '<tr class=dc><td><center>';

 $pg .= makeForm('settings');
 $pg .= '<table cellpadding=5 cellspacing=0 border=0>';
 $pg .= '<tr class=dc><td class=dr colspan=2>';
 $pg .= 'To change your payout address, enter a new address and your password';
 $pg .= '</td></tr>';
 $pg .= '<tr class=dc><td class=dr>';
 $pg .= 'Address:';
 $pg .= '</td><td class=dl>';
 $pg .= "<input type=text name=addr value='$addr' size=42>";
 $pg .= '</td></tr>';
 $pg .= '<tr class=dc><td class=dr>';
 $pg .= 'Password:';
 $pg .= '</td><td class=dl>';
 $pg .= '<input type=password name=pass size=20>';
 $pg .= '</td></tr>';
 $pg .= '<tr class=dc><td class=dr colspan=2>';
 $pg .= 'Change: <input type=submit name=Change value=Address>';
 $pg .= '</td></tr>';
 $pg .= '</table></form>';

 $pg .= '</center></td></tr>';
 $pg .= '<tr class=dc><td><center>';

 $pg .= makeForm('settings');
 $pg .= '<table cellpadding=5 cellspacing=0 border=0>';
 $pg .= '<tr class=dc><td class=dr colspan=2>';
 $pg .= 'To change your password, enter your old password and new password twice';
 $pg .= '</td></tr>';
 $pg .= '<tr class=dc><td class=dr nowrap>';
 $pg .= 'Old Password:';
 $pg .= '</td><td class=dl>';
 $pg .= "<input type=password name=oldpass size=20>";
 $pg .= '</td></tr>';
 $pg .= '<tr class=dc><td class=dr nowrap>';
 $pg .= 'New Password:';
 $pg .= '</td><td class=dl>';
 $pg .= '<input type=password name=pass1 size=20>';
 $pg .= '</td></tr>';
 $pg .= '<tr class=dc><td class=dr nowrap>';
 $pg .= 'New Password again:';
 $pg .= '</td><td class=dl>';
 $pg .= '<input type=password name=pass2 size=20>';
 $pg .= '</td></tr>';
 $pg .= '<tr class=dc><td class=dr colspan=2>';
 $pg .= 'Change: <input type=submit name=Change value=Password>';
 $pg .= '</td></tr>';
 $pg .= '</table></form>';

 $pg .= '</center></td></tr>';
 $pg .= '</table>';

 return $pg;
}
#
function dosettings($data, $user)
{
 $err = '';
 $chg = getparam('Change', false);
 $check = false;
 switch ($chg)
 {
  case 'EMail':
	$email = getparam('email', false);
	$pass = getparam('pass', false);
	$ans = userSettings($user, $email, null, $pass);
	$check = true;
	break;
  case 'Address':
	$addr = getparam('addr', false);
	$pass = getparam('pass', false);
	$ans = userSettings($user, null, $addr, $pass);
	$check = true;
	break;
  case 'Password':
	$oldpass = getparam('oldpass', false);
	$pass1 = getparam('pass1', false);
	$pass2 = getparam('pass2', false);
	if (!safepass($pass1))
	{
		$err = "Password is unsafe - requires 6 or more characters, including<br>" .
			"at least one of each uppercase, lowercase and digits, but not Tab";
	}
	elseif ($pass1 != $pass2)
		$err = "Passwords don't match";
	else
	{
		$ans = setPass($user, $oldpass, $pass1);
		$err = 'Password changed';
		$check = true;
	}
	break;
 }
 if ($check === true)
	if ($ans['STATUS'] != 'ok')
	{
		$err = $ans['STATUS'];
		if ($ans['ERROR'] != '')
			$err .= ': '.$ans['ERROR'];
	}
 $ans = userSettings($user);
 if ($ans['STATUS'] != 'ok')
	dbdown(); // Should be no other reason?
 if (isset($ans['email']))
	$email = $ans['email'];
 else
	$email = '';
 if (isset($ans['addr']))
	$addr = $ans['addr'];
 else
	$addr = '';
 $pg = settings($data, $user, $email, $addr, $err);
 return $pg;
}
#
function show_settings($page, $menu, $name, $user)
{
 gopage(NULL, 'dosettings', $page, $menu, $name, $user);
}
#
?>
