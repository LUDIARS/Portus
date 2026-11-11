#include "portus/aws.hpp"
#include "test.hpp"

using namespace portus;

//署名値は秘密鍵とスコープだけで決まり、アクセスキー ID は Credential= に載るだけで計算に入らない。
//そのため ID はキー形式でないダミーにし、署名値は AWS ドキュメントの例 (S3 GET Object) と照合する
TEST(sigv4_aws_example, "SigV4: AWS ドキュメントの S3 GET Object 例と一致する") {
	SignInput input;
	input.method = "GET";
	input.host = "examplebucket.s3.amazonaws.com";
	input.path = "/test.txt";
	input.region = "us-east-1";
	input.service = "s3";
	input.headers = { { "Range", "bytes=0-9" } };
	input.date_ms = 1369353600000LL;	//2013-05-24T00:00:00Z

	AwsCredentials creds;
	creds.access_key_id = "TEST-ACCESS-KEY-ID";
	creds.secret_access_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";

	const HeaderList headers = sign_request(input, creds);
	std::string authorization;
	std::string date;
	for (const auto& kv : headers) {
		if (kv.first == "authorization") authorization = kv.second;
		if (kv.first == "x-amz-date") date = kv.second;
		CHECK(kv.first != "host");
	}
	CHECK_EQ(date, "20130524T000000Z");
	CHECK_EQ(authorization,
		"AWS4-HMAC-SHA256 Credential=TEST-ACCESS-KEY-ID/20130524/us-east-1/s3/aws4_request, "
		"SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
		"Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
}

TEST(sigv4_key_path, "SigV4: S3 キーのエンコード (区切りは残し、予約文字はエンコードする)") {
	CHECK_EQ(encode_key_path("logs/a b/あ(1)*.txt"), "logs/a%20b/%E3%81%82%281%29%2A.txt");
	CHECK_EQ(encode_key_path("a-b_c.d~e"), "a-b_c.d~e");
}

TEST(aws_credentials_file, "AWS: 共有認証ファイルから指定プロファイルを読む") {
	const std::string text = "[default]\naws_access_key_id = A\naws_secret_access_key = B\n\n; comment\n[other]\r\naws_access_key_id=C\r\naws_secret_access_key=D\r\naws_session_token=E\r\n";
	const auto other = parse_credentials_file(text, "other");
	CHECK(other.has_value());
	CHECK_EQ(other->access_key_id, "C");
	CHECK_EQ(other->secret_access_key, "D");
	CHECK_EQ(other->session_token, "E");
	CHECK_EQ(parse_credentials_file(text, "default")->secret_access_key, "B");
	CHECK(!parse_credentials_file(text, "none").has_value());
}

TEST(s3_rejects_bad_names, "S3: ホスト名に埋め込むバケット名・リージョンを検証する") {
	CHECK_THROWS(put_object("ap-northeast-1", "Bad_Bucket", "k", "x"));
	CHECK_THROWS(put_object("ap-northeast-1", "a.evil.example/x", "k", "x"));
	CHECK_THROWS(put_object("evil.example/", "my-bucket", "k", "x"));
}
