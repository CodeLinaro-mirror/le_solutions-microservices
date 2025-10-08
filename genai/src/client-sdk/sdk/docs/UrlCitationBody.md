# UrlCitationBody

A citation for a web resource used to generate a model response.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the URL citation. Always &#x60;url_citation&#x60;. | [default to 'url_citation']
**url** | **str** | The URL of the web resource. | 
**start_index** | **int** | The index of the first character of the URL citation in the message. | 
**end_index** | **int** | The index of the last character of the URL citation in the message. | 
**title** | **str** | The title of the web resource. | 

## Example

```python
from openapi_client.models.url_citation_body import UrlCitationBody

# TODO update the JSON string below
json = "{}"
# create an instance of UrlCitationBody from a JSON string
url_citation_body_instance = UrlCitationBody.from_json(json)
# print the JSON string representation of the object
print(UrlCitationBody.to_json())

# convert the object into a dict
url_citation_body_dict = url_citation_body_instance.to_dict()
# create an instance of UrlCitationBody from a dict
url_citation_body_from_dict = UrlCitationBody.from_dict(url_citation_body_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


